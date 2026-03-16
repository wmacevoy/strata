/*
 * msg.c — Minimal TCP transport layer.
 *
 * REQ/REP: persistent TCP connections, length-prefixed messages.
 * PUB/SUB: publisher accepts subscribers, server-side topic filtering.
 * Fork-safe: just file descriptors, no threads, no shared state.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "strata/msg.h"

/* ---- Internal structures ---- */

struct strata_sock {
    int fd;           /* connected socket or listen socket */
    int recv_ms;      /* receive timeout (-1 = block) */
    int send_ms;      /* send timeout (-1 = block) */
    char sub_prefix[256]; /* subscription filter (SUB only) */
    int sub_prefix_len;
    int is_listener;  /* 1 if this is a listen socket (REP) */
    int proxied;      /* 1 if ops go through kernel pipe */
    uint16_t handle;  /* capability handle (proxied mode) */
};

#define MAX_SUBSCRIBERS 64

typedef struct {
    int fd;
    char prefix[256];
    int prefix_len;
} sub_entry;

struct strata_pub_hub {
    int listen_fd;
    sub_entry subs[MAX_SUBSCRIBERS];
    int sub_count;
    int proxied;       /* kernel pipe mode */
    uint16_t handle;   /* handle in proxy */
};

/* ---- Endpoint parsing ---- */

int strata_endpoint_parse(const char *endpoint,
                          char *host_buf, size_t host_cap,
                          int *port) {
    /* "tcp://host:port" */
    if (!endpoint) return -1;
    const char *p = endpoint;
    if (strncmp(p, "tcp://", 6) == 0) p += 6;

    const char *colon = strrchr(p, ':');
    if (!colon) return -1;

    size_t hlen = colon - p;
    if (hlen >= host_cap) return -1;
    memcpy(host_buf, p, hlen);
    host_buf[hlen] = '\0';

    *port = atoi(colon + 1);
    if (*port <= 0 || *port > 65535) return -1;
    return 0;
}

/* ---- Low-level I/O ---- */

/* Write exactly n bytes. Returns 0 on success, -1 on error. */
static int write_all(int fd, const void *buf, size_t n, int timeout_ms) {
    const uint8_t *p = buf;
    size_t remaining = n;
    while (remaining > 0) {
        if (timeout_ms >= 0) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            int r = poll(&pfd, 1, timeout_ms);
            if (r <= 0) return -1;
        }
        ssize_t w = write(fd, p, remaining);
        if (w <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        remaining -= w;
    }
    return 0;
}

/* Read exactly n bytes. Returns 0 on success, -1 on error/timeout. */
static int read_all(int fd, void *buf, size_t n, int timeout_ms) {
    uint8_t *p = buf;
    size_t remaining = n;
    while (remaining > 0) {
        if (timeout_ms >= 0) {
            struct pollfd pfd = {fd, POLLIN, 0};
            int r = poll(&pfd, 1, timeout_ms);
            if (r <= 0) return -1;
        }
        ssize_t r = read(fd, p, remaining);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        p += r;
        remaining -= r;
    }
    return 0;
}

/* Send a length-prefixed frame: [4-byte big-endian len][payload] */
static int send_frame(int fd, const void *buf, size_t len, int timeout_ms) {
    uint32_t net_len = htonl((uint32_t)len);
    if (write_all(fd, &net_len, 4, timeout_ms) != 0) return -1;
    if (len > 0 && write_all(fd, buf, len, timeout_ms) != 0) return -1;
    return (int)len;
}

/* Receive a length-prefixed frame into caller-allocated buffer.
 * Returns payload length or -1. */
static int recv_frame_alloc(int fd, char **out, int timeout_ms) {
    uint32_t net_len;
    if (read_all(fd, &net_len, 4, timeout_ms) != 0) return -1;
    uint32_t len = ntohl(net_len);
    if (len > 16 * 1024 * 1024) return -1; /* 16MB sanity limit */
    char *buf = malloc(len + 1);
    if (!buf) return -1;
    if (len > 0 && read_all(fd, buf, len, timeout_ms) != 0) {
        free(buf);
        return -1;
    }
    buf[len] = '\0';
    *out = buf;
    return (int)len;
}

/* Receive a length-prefixed frame. Returns payload length or -1. */
static int recv_frame(int fd, void *buf, size_t cap, int timeout_ms) {
    uint32_t net_len;
    if (read_all(fd, &net_len, 4, timeout_ms) != 0) return -1;
    uint32_t len = ntohl(net_len);
    if (len > cap) {
        /* message too large — drain and discard */
        uint8_t discard[256];
        uint32_t remaining = len;
        while (remaining > 0) {
            uint32_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
            if (read_all(fd, discard, chunk, timeout_ms) != 0) return -1;
            remaining -= chunk;
        }
        return -1;
    }
    if (len > 0 && read_all(fd, buf, len, timeout_ms) != 0) return -1;
    return (int)len;
}

/* Create a listening TCP socket on host:port. Returns fd or -1. */
static int tcp_listen(const char *host, int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (strcmp(host, "0.0.0.0") == 0 || strcmp(host, "*") == 0)
        addr.sin_addr.s_addr = INADDR_ANY;
    else
        inet_pton(AF_INET, host, &addr.sin_addr);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Connect to host:port. Returns fd or -1. */
static int tcp_connect(const char *host, int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Create a strata_sock wrapping an fd. */
static strata_sock *sock_new(int fd) {
    strata_sock *s = calloc(1, sizeof(strata_sock));
    if (!s) { close(fd); return NULL; }
    s->fd = fd;
    s->recv_ms = -1;
    s->send_ms = -1;
    return s;
}

/* ---- Kernel pipe protocol ---- */

static int g_kernel_fd = -1;

enum {
    OP_REQ_CONNECT = 1,
    OP_SEND,
    OP_RECV,
    OP_CLOSE,
    OP_SET_TIMEOUT,
    OP_REP_BIND,
    OP_REP_ACCEPT,
    OP_SUB_CONNECT,
    OP_SUBSCRIBE,
    OP_SUB_RECV,
    OP_PUB_BIND,
    OP_PUB_SEND,
    OP_PUB_CLOSE,
};

typedef struct {
    uint16_t op;
    uint16_t handle;
    uint32_t len;
} kernel_req_t;

typedef struct {
    int32_t  rc;
    uint32_t len;
} kernel_resp_t;

void strata_msg_set_kernel(int pipe_fd) {
    g_kernel_fd = pipe_fd;
}

static int kernel_send_req(uint16_t op, uint16_t handle,
                           const void *payload, uint32_t len) {
    kernel_req_t hdr;
    hdr.op = htons(op);
    hdr.handle = htons(handle);
    hdr.len = htonl(len);
    if (write_all(g_kernel_fd, &hdr, sizeof(hdr), -1) != 0) return -1;
    if (len > 0 && write_all(g_kernel_fd, payload, len, -1) != 0) return -1;
    return 0;
}

static int kernel_recv_resp(kernel_resp_t *resp) {
    if (read_all(g_kernel_fd, resp, sizeof(*resp), -1) != 0) return -1;
    resp->rc = (int32_t)ntohl((uint32_t)resp->rc);
    resp->len = ntohl(resp->len);
    return 0;
}

static int kernel_drain(uint32_t len) {
    char tmp[256];
    while (len > 0) {
        uint32_t chunk = len < sizeof(tmp) ? len : sizeof(tmp);
        if (read_all(g_kernel_fd, tmp, chunk, -1) != 0) return -1;
        len -= chunk;
    }
    return 0;
}

static strata_sock *kernel_sock_new(uint16_t handle) {
    strata_sock *s = calloc(1, sizeof(strata_sock));
    if (!s) return NULL;
    s->fd = -1;
    s->proxied = 1;
    s->handle = handle;
    s->recv_ms = -1;
    s->send_ms = -1;
    return s;
}

/* ---- REQ/REP ---- */

strata_sock *strata_rep_bind(const char *endpoint) {
    if (g_kernel_fd >= 0) {
        size_t elen = endpoint ? strlen(endpoint) : 0;
        if (kernel_send_req(OP_REP_BIND, 0, endpoint, (uint32_t)elen) != 0)
            return NULL;
        kernel_resp_t resp;
        if (kernel_recv_resp(&resp) != 0) return NULL;
        if (resp.len > 0) kernel_drain(resp.len);
        if (resp.rc < 0) return NULL;
        strata_sock *s = kernel_sock_new((uint16_t)resp.rc);
        if (s) s->is_listener = 1;
        return s;
    }

    char host[256];
    int port;
    if (strata_endpoint_parse(endpoint, host, sizeof(host), &port) != 0)
        return NULL;

    int fd = tcp_listen(host, port);
    if (fd < 0) return NULL;

    strata_sock *s = sock_new(fd);
    if (s) s->is_listener = 1;
    return s;
}

strata_sock *strata_rep_accept(strata_sock *rep) {
    if (!rep) return NULL;
    if (rep->proxied) {
        if (kernel_send_req(OP_REP_ACCEPT, rep->handle, NULL, 0) != 0)
            return NULL;
        kernel_resp_t resp;
        if (kernel_recv_resp(&resp) != 0) return NULL;
        if (resp.len > 0) kernel_drain(resp.len);
        if (resp.rc < 0) return NULL;
        return kernel_sock_new((uint16_t)resp.rc);
    }

    if (!rep->is_listener) return NULL;

    struct pollfd pfd = {rep->fd, POLLIN, 0};
    int timeout = rep->recv_ms >= 0 ? rep->recv_ms : -1;
    int r = poll(&pfd, 1, timeout);
    if (r <= 0) return NULL;

    int client_fd = accept(rep->fd, NULL, NULL);
    if (client_fd < 0) return NULL;

    int opt = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    strata_sock *cs = sock_new(client_fd);
    if (cs) {
        cs->recv_ms = rep->recv_ms;
        cs->send_ms = rep->send_ms;
    }
    return cs;
}

strata_sock *strata_req_connect(const char *endpoint) {
    if (g_kernel_fd >= 0) {
        size_t elen = endpoint ? strlen(endpoint) : 0;
        if (kernel_send_req(OP_REQ_CONNECT, 0, endpoint, (uint32_t)elen) != 0)
            return NULL;
        kernel_resp_t resp;
        if (kernel_recv_resp(&resp) != 0) return NULL;
        if (resp.len > 0) kernel_drain(resp.len);
        if (resp.rc < 0) return NULL;
        return kernel_sock_new((uint16_t)resp.rc);
    }

    char host[256];
    int port;
    if (strata_endpoint_parse(endpoint, host, sizeof(host), &port) != 0)
        return NULL;

    int fd = tcp_connect(host, port);
    if (fd < 0) return NULL;

    return sock_new(fd);
}

int strata_msg_send(strata_sock *sock, const void *buf, size_t len, int flags) {
    if (!sock) return -1;
    (void)flags;
    if (sock->proxied) {
        if (kernel_send_req(OP_SEND, sock->handle, buf, (uint32_t)len) != 0)
            return -1;
        kernel_resp_t resp;
        if (kernel_recv_resp(&resp) != 0) return -1;
        if (resp.len > 0) kernel_drain(resp.len);
        return resp.rc;
    }
    if (sock->fd < 0) return -1;
    return send_frame(sock->fd, buf, len, sock->send_ms);
}

int strata_msg_recv(strata_sock *sock, void *buf, size_t cap, int flags) {
    if (!sock) return -1;
    (void)flags;
    if (sock->proxied) {
        if (kernel_send_req(OP_RECV, sock->handle, NULL, 0) != 0)
            return -1;
        kernel_resp_t resp;
        if (kernel_recv_resp(&resp) != 0) return -1;
        if (resp.rc < 0) {
            if (resp.len > 0) kernel_drain(resp.len);
            return -1;
        }
        if (resp.len > cap) {
            kernel_drain(resp.len);
            return -1;
        }
        if (resp.len > 0 && read_all(g_kernel_fd, buf, resp.len, -1) != 0)
            return -1;
        return resp.rc;
    }
    if (sock->fd < 0) return -1;
    return recv_frame(sock->fd, buf, cap, sock->recv_ms);
}

int strata_msg_recv_alloc(strata_sock *sock, char **out, int flags) {
    if (!sock || !out) return -1;
    *out = NULL;
    (void)flags;
    if (sock->proxied) {
        if (kernel_send_req(OP_RECV, sock->handle, NULL, 0) != 0)
            return -1;
        kernel_resp_t resp;
        if (kernel_recv_resp(&resp) != 0) return -1;
        if (resp.rc < 0) {
            if (resp.len > 0) kernel_drain(resp.len);
            return -1;
        }
        char *buf = malloc(resp.len + 1);
        if (!buf) { kernel_drain(resp.len); return -1; }
        if (resp.len > 0 && read_all(g_kernel_fd, buf, resp.len, -1) != 0) {
            free(buf);
            return -1;
        }
        buf[resp.len] = '\0';
        *out = buf;
        return resp.rc;
    }
    if (sock->fd < 0) return -1;
    return recv_frame_alloc(sock->fd, out, sock->recv_ms);
}

void strata_msg_set_timeout(strata_sock *sock, int recv_ms, int send_ms) {
    if (!sock) return;
    if (sock->proxied) {
        uint32_t payload[2];
        payload[0] = htonl((uint32_t)recv_ms);
        payload[1] = htonl((uint32_t)send_ms);
        kernel_send_req(OP_SET_TIMEOUT, sock->handle, payload, 8);
        kernel_resp_t resp;
        kernel_recv_resp(&resp);
        if (resp.len > 0) kernel_drain(resp.len);
        return;
    }
    sock->recv_ms = recv_ms;
    sock->send_ms = send_ms;
}

void strata_sock_close(strata_sock *sock) {
    if (!sock) return;
    if (sock->proxied) {
        kernel_send_req(OP_CLOSE, sock->handle, NULL, 0);
        kernel_resp_t resp;
        kernel_recv_resp(&resp);
        if (resp.len > 0) kernel_drain(resp.len);
        free(sock);
        return;
    }
    if (sock->fd >= 0) close(sock->fd);
    free(sock);
}

int strata_sock_fd(strata_sock *sock) {
    if (!sock || sock->proxied) return -1;
    return sock->fd;
}

/* ---- PUB/SUB ---- */

strata_pub_hub *strata_pub_bind(const char *endpoint) {
    if (g_kernel_fd >= 0) {
        size_t elen = endpoint ? strlen(endpoint) : 0;
        if (kernel_send_req(OP_PUB_BIND, 0, endpoint, (uint32_t)elen) != 0)
            return NULL;
        kernel_resp_t resp;
        if (kernel_recv_resp(&resp) != 0) return NULL;
        if (resp.len > 0) kernel_drain(resp.len);
        if (resp.rc < 0) return NULL;
        strata_pub_hub *pub = calloc(1, sizeof(strata_pub_hub));
        if (!pub) return NULL;
        pub->proxied = 1;
        pub->handle = (uint16_t)resp.rc;
        pub->listen_fd = -1;
        return pub;
    }

    char host[256];
    int port;
    if (strata_endpoint_parse(endpoint, host, sizeof(host), &port) != 0)
        return NULL;

    int fd = tcp_listen(host, port);
    if (fd < 0) return NULL;

    /* Set listen socket non-blocking for inline accept in pub_send */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    strata_pub_hub *pub = calloc(1, sizeof(strata_pub_hub));
    if (!pub) { close(fd); return NULL; }
    pub->listen_fd = fd;
    return pub;
}

/* Accept any pending subscribers (non-blocking). */
static void pub_accept_pending(strata_pub_hub *pub) {
    for (;;) {
        int fd = accept(pub->listen_fd, NULL, NULL);
        if (fd < 0) break;  /* no more pending */

        if (pub->sub_count >= MAX_SUBSCRIBERS) {
            close(fd);
            continue;
        }

        /* Read subscription registration: [4-byte prefix_len][prefix] */
        uint32_t net_plen;
        if (read_all(fd, &net_plen, 4, 2000) != 0) {
            close(fd);
            continue;
        }
        uint32_t plen = ntohl(net_plen);
        if (plen >= 256) { close(fd); continue; }

        sub_entry *e = &pub->subs[pub->sub_count];
        e->fd = fd;
        e->prefix_len = (int)plen;
        if (plen > 0) {
            if (read_all(fd, e->prefix, plen, 2000) != 0) {
                close(fd);
                continue;
            }
        }
        e->prefix[plen] = '\0';

        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        pub->sub_count++;
    }
}

int strata_pub_send(strata_pub_hub *pub,
                    const char *topic, size_t topic_len,
                    const void *payload, size_t payload_len) {
    if (!pub) return -1;

    if (pub->proxied) {
        /* Pack: [2-byte topic_len][topic][payload] */
        size_t total = 2 + topic_len + payload_len;
        char *buf = malloc(total);
        if (!buf) return -1;
        uint16_t net_tlen = htons((uint16_t)topic_len);
        memcpy(buf, &net_tlen, 2);
        if (topic_len > 0) memcpy(buf + 2, topic, topic_len);
        if (payload_len > 0) memcpy(buf + 2 + topic_len, payload, payload_len);

        int r = kernel_send_req(OP_PUB_SEND, pub->handle, buf, (uint32_t)total);
        free(buf);
        if (r != 0) return -1;

        kernel_resp_t resp;
        if (kernel_recv_resp(&resp) != 0) return -1;
        if (resp.len > 0) kernel_drain(resp.len);
        return resp.rc;
    }

    /* Pick up new subscribers */
    pub_accept_pending(pub);

    int reached = 0;
    for (int i = 0; i < pub->sub_count; i++) {
        sub_entry *e = &pub->subs[i];

        /* Topic filter: subscriber's prefix must match */
        if (e->prefix_len > 0) {
            if ((int)topic_len < e->prefix_len) continue;
            if (memcmp(topic, e->prefix, e->prefix_len) != 0) continue;
        }

        /* Send: [4-byte topic_len][topic][4-byte payload_len][payload] */
        uint32_t net_tlen = htonl((uint32_t)topic_len);
        uint32_t net_plen = htonl((uint32_t)payload_len);

        int ok = 1;
        if (write_all(e->fd, &net_tlen, 4, 1000) != 0) ok = 0;
        if (ok && topic_len > 0 && write_all(e->fd, topic, topic_len, 1000) != 0) ok = 0;
        if (ok && write_all(e->fd, &net_plen, 4, 1000) != 0) ok = 0;
        if (ok && payload_len > 0 && write_all(e->fd, payload, payload_len, 1000) != 0) ok = 0;

        if (ok) {
            reached++;
        } else {
            /* Dead subscriber — close and remove */
            close(e->fd);
            pub->subs[i] = pub->subs[pub->sub_count - 1];
            pub->sub_count--;
            i--;
        }
    }
    return reached;
}

void strata_pub_close(strata_pub_hub *pub) {
    if (!pub) return;
    if (pub->proxied) {
        kernel_send_req(OP_PUB_CLOSE, pub->handle, NULL, 0);
        kernel_resp_t resp;
        kernel_recv_resp(&resp);
        if (resp.len > 0) kernel_drain(resp.len);
        free(pub);
        return;
    }
    for (int i = 0; i < pub->sub_count; i++)
        close(pub->subs[i].fd);
    if (pub->listen_fd >= 0) close(pub->listen_fd);
    free(pub);
}

strata_sock *strata_sub_connect(const char *endpoint) {
    if (g_kernel_fd >= 0) {
        size_t elen = endpoint ? strlen(endpoint) : 0;
        if (kernel_send_req(OP_SUB_CONNECT, 0, endpoint, (uint32_t)elen) != 0)
            return NULL;
        kernel_resp_t resp;
        if (kernel_recv_resp(&resp) != 0) return NULL;
        if (resp.len > 0) kernel_drain(resp.len);
        if (resp.rc < 0) return NULL;
        return kernel_sock_new((uint16_t)resp.rc);
    }

    char host[256];
    int port;
    if (strata_endpoint_parse(endpoint, host, sizeof(host), &port) != 0)
        return NULL;

    int fd = tcp_connect(host, port);
    if (fd < 0) return NULL;

    return sock_new(fd);
}

int strata_sub_subscribe(strata_sock *sock, const char *prefix) {
    if (!sock) return -1;

    if (sock->proxied) {
        size_t plen = prefix ? strlen(prefix) : 0;
        if (kernel_send_req(OP_SUBSCRIBE, sock->handle,
                            prefix, (uint32_t)plen) != 0)
            return -1;
        kernel_resp_t resp;
        if (kernel_recv_resp(&resp) != 0) return -1;
        if (resp.len > 0) kernel_drain(resp.len);
        return resp.rc;
    }

    if (sock->fd < 0) return -1;

    size_t plen = prefix ? strlen(prefix) : 0;
    if (plen >= 256) return -1;

    /* Store locally */
    memcpy(sock->sub_prefix, prefix ? prefix : "", plen);
    sock->sub_prefix[plen] = '\0';
    sock->sub_prefix_len = (int)plen;

    /* Send registration to publisher: [4-byte prefix_len][prefix] */
    uint32_t net_plen = htonl((uint32_t)plen);
    if (write_all(sock->fd, &net_plen, 4, 5000) != 0) return -1;
    if (plen > 0 && write_all(sock->fd, prefix, plen, 5000) != 0) return -1;

    return 0;
}

int strata_sub_recv(strata_sock *sock,
                    char *topic_buf, size_t topic_cap,
                    void *payload_buf, size_t payload_cap) {
    if (!sock) return -1;

    if (sock->proxied) {
        if (kernel_send_req(OP_SUB_RECV, sock->handle, NULL, 0) != 0)
            return -1;
        kernel_resp_t resp;
        if (kernel_recv_resp(&resp) != 0) return -1;
        if (resp.rc < 0) {
            if (resp.len > 0) kernel_drain(resp.len);
            return -1;
        }
        /* Response payload: [2-byte topic_len][topic][payload] */
        if (resp.len < 2) { if (resp.len > 0) kernel_drain(resp.len); return -1; }
        uint16_t net_tlen;
        if (read_all(g_kernel_fd, &net_tlen, 2, -1) != 0) return -1;
        uint16_t tlen = ntohs(net_tlen);
        uint32_t remaining = resp.len - 2;
        if (tlen > remaining || tlen >= topic_cap) {
            kernel_drain(remaining);
            return -1;
        }
        if (tlen > 0 && read_all(g_kernel_fd, topic_buf, tlen, -1) != 0)
            return -1;
        topic_buf[tlen] = '\0';
        remaining -= tlen;
        if (remaining > payload_cap) {
            kernel_drain(remaining);
            return -1;
        }
        if (remaining > 0 && read_all(g_kernel_fd, payload_buf, remaining, -1) != 0)
            return -1;
        return (int)remaining;
    }

    if (sock->fd < 0) return -1;

    /* Read: [4-byte topic_len][topic][4-byte payload_len][payload] */
    uint32_t net_tlen;
    if (read_all(sock->fd, &net_tlen, 4, sock->recv_ms) != 0) return -1;
    uint32_t tlen = ntohl(net_tlen);

    if (tlen >= topic_cap) return -1;  /* topic too large */
    if (tlen > 0 && read_all(sock->fd, topic_buf, tlen, sock->recv_ms) != 0) return -1;
    topic_buf[tlen] = '\0';

    uint32_t net_plen;
    if (read_all(sock->fd, &net_plen, 4, sock->recv_ms) != 0) return -1;
    uint32_t plen = ntohl(net_plen);

    if (plen > payload_cap) return -1;  /* payload too large */
    if (plen > 0 && read_all(sock->fd, payload_buf, plen, sock->recv_ms) != 0) return -1;

    return (int)plen;
}

/* ---- Kernel proxy loop ---- */

#define PROXY_MAX_HANDLES 64
#define PROXY_MAX_PUB      8
#define PROXY_MAX_RECV  (4 * 1024 * 1024)

static int proxy_check_permit(const strata_msg_permit *permits, int npermits,
                              int kind, const char *endpoint) {
    for (int i = 0; i < npermits; i++) {
        if (permits[i].kind == kind &&
            endpoint && strcmp(permits[i].endpoint, endpoint) == 0)
            return 1;
    }
    return 0;
}

static int proxy_alloc_handle(strata_sock **handles) {
    for (int i = 0; i < PROXY_MAX_HANDLES; i++)
        if (!handles[i]) return i;
    return -1;
}

static int proxy_alloc_pub(strata_pub_hub **pub_handles) {
    for (int i = 0; i < PROXY_MAX_PUB; i++)
        if (!pub_handles[i]) return i;
    return -1;
}

static void proxy_send_resp(int pipe_fd, int32_t rc,
                            const void *data, uint32_t len) {
    kernel_resp_t resp;
    resp.rc = (int32_t)htonl((uint32_t)rc);
    resp.len = htonl(len);
    write_all(pipe_fd, &resp, sizeof(resp), -1);
    if (len > 0 && data)
        write_all(pipe_fd, data, len, -1);
}

int strata_msg_proxy_run(int pipe_fd,
                         const strata_msg_permit *permits, int npermits) {
    strata_sock *handles[PROXY_MAX_HANDLES] = {0};
    strata_pub_hub *pub_handles[PROXY_MAX_PUB] = {0};

    kernel_req_t req;
    while (read_all(pipe_fd, &req, sizeof(req), -1) == 0) {
        uint16_t op = ntohs(req.op);
        uint16_t handle = ntohs(req.handle);
        uint32_t len = ntohl(req.len);

        /* Read payload */
        char *payload = NULL;
        if (len > 0) {
            if (len > PROXY_MAX_RECV) break;
            payload = malloc(len + 1);
            if (!payload) break;
            if (read_all(pipe_fd, payload, len, -1) != 0) {
                free(payload);
                break;
            }
            payload[len] = '\0';
        }

        switch (op) {

        case OP_REQ_CONNECT: {
            if (!proxy_check_permit(permits, npermits,
                                    STRATA_CAP_REQ, payload)) {
                proxy_send_resp(pipe_fd, -1, NULL, 0);
                break;
            }
            strata_sock *s = strata_req_connect(payload);
            if (!s) { proxy_send_resp(pipe_fd, -1, NULL, 0); break; }
            int h = proxy_alloc_handle(handles);
            if (h < 0) {
                strata_sock_close(s);
                proxy_send_resp(pipe_fd, -1, NULL, 0);
                break;
            }
            handles[h] = s;
            proxy_send_resp(pipe_fd, h, NULL, 0);
            break;
        }

        case OP_SEND: {
            if (handle >= PROXY_MAX_HANDLES || !handles[handle]) {
                proxy_send_resp(pipe_fd, -1, NULL, 0);
                break;
            }
            int rc = strata_msg_send(handles[handle], payload, len, 0);
            proxy_send_resp(pipe_fd, rc, NULL, 0);
            break;
        }

        case OP_RECV: {
            if (handle >= PROXY_MAX_HANDLES || !handles[handle]) {
                proxy_send_resp(pipe_fd, -1, NULL, 0);
                break;
            }
            char *buf = malloc(PROXY_MAX_RECV);
            if (!buf) { proxy_send_resp(pipe_fd, -1, NULL, 0); break; }
            int rc = strata_msg_recv(handles[handle], buf, PROXY_MAX_RECV, 0);
            if (rc < 0) {
                proxy_send_resp(pipe_fd, -1, NULL, 0);
            } else {
                proxy_send_resp(pipe_fd, rc, buf, (uint32_t)rc);
            }
            free(buf);
            break;
        }

        case OP_CLOSE: {
            if (handle < PROXY_MAX_HANDLES && handles[handle]) {
                strata_sock_close(handles[handle]);
                handles[handle] = NULL;
            }
            proxy_send_resp(pipe_fd, 0, NULL, 0);
            break;
        }

        case OP_SET_TIMEOUT: {
            if (handle < PROXY_MAX_HANDLES && handles[handle] && len >= 8) {
                uint32_t *vals = (uint32_t *)payload;
                int recv_ms = (int32_t)ntohl(vals[0]);
                int send_ms = (int32_t)ntohl(vals[1]);
                strata_msg_set_timeout(handles[handle], recv_ms, send_ms);
            }
            proxy_send_resp(pipe_fd, 0, NULL, 0);
            break;
        }

        case OP_REP_BIND: {
            if (!proxy_check_permit(permits, npermits,
                                    STRATA_CAP_REP, payload)) {
                proxy_send_resp(pipe_fd, -1, NULL, 0);
                break;
            }
            strata_sock *s = strata_rep_bind(payload);
            if (!s) { proxy_send_resp(pipe_fd, -1, NULL, 0); break; }
            int h = proxy_alloc_handle(handles);
            if (h < 0) {
                strata_sock_close(s);
                proxy_send_resp(pipe_fd, -1, NULL, 0);
                break;
            }
            handles[h] = s;
            proxy_send_resp(pipe_fd, h, NULL, 0);
            break;
        }

        case OP_REP_ACCEPT: {
            if (handle >= PROXY_MAX_HANDLES || !handles[handle]) {
                proxy_send_resp(pipe_fd, -1, NULL, 0);
                break;
            }
            strata_sock *client = strata_rep_accept(handles[handle]);
            if (!client) { proxy_send_resp(pipe_fd, -1, NULL, 0); break; }
            int h = proxy_alloc_handle(handles);
            if (h < 0) {
                strata_sock_close(client);
                proxy_send_resp(pipe_fd, -1, NULL, 0);
                break;
            }
            handles[h] = client;
            proxy_send_resp(pipe_fd, h, NULL, 0);
            break;
        }

        case OP_SUB_CONNECT: {
            if (!proxy_check_permit(permits, npermits,
                                    STRATA_CAP_SUB, payload)) {
                proxy_send_resp(pipe_fd, -1, NULL, 0);
                break;
            }
            strata_sock *s = strata_sub_connect(payload);
            if (!s) { proxy_send_resp(pipe_fd, -1, NULL, 0); break; }
            int h = proxy_alloc_handle(handles);
            if (h < 0) {
                strata_sock_close(s);
                proxy_send_resp(pipe_fd, -1, NULL, 0);
                break;
            }
            handles[h] = s;
            proxy_send_resp(pipe_fd, h, NULL, 0);
            break;
        }

        case OP_SUBSCRIBE: {
            if (handle >= PROXY_MAX_HANDLES || !handles[handle]) {
                proxy_send_resp(pipe_fd, -1, NULL, 0);
                break;
            }
            int rc = strata_sub_subscribe(handles[handle],
                                          payload ? payload : "");
            proxy_send_resp(pipe_fd, rc, NULL, 0);
            break;
        }

        case OP_SUB_RECV: {
            if (handle >= PROXY_MAX_HANDLES || !handles[handle]) {
                proxy_send_resp(pipe_fd, -1, NULL, 0);
                break;
            }
            char topic[512] = {0};
            char pbuf[8192] = {0};
            int rc = strata_sub_recv(handles[handle],
                                     topic, sizeof(topic),
                                     pbuf, sizeof(pbuf));
            if (rc < 0) {
                proxy_send_resp(pipe_fd, -1, NULL, 0);
            } else {
                uint16_t tlen = (uint16_t)strlen(topic);
                uint32_t total = 2 + tlen + (uint32_t)rc;
                char *resp_buf = malloc(total);
                if (!resp_buf) {
                    proxy_send_resp(pipe_fd, -1, NULL, 0);
                    break;
                }
                uint16_t net_tlen = htons(tlen);
                memcpy(resp_buf, &net_tlen, 2);
                memcpy(resp_buf + 2, topic, tlen);
                memcpy(resp_buf + 2 + tlen, pbuf, rc);
                proxy_send_resp(pipe_fd, rc, resp_buf, total);
                free(resp_buf);
            }
            break;
        }

        case OP_PUB_BIND: {
            if (!proxy_check_permit(permits, npermits,
                                    STRATA_CAP_PUB, payload)) {
                proxy_send_resp(pipe_fd, -1, NULL, 0);
                break;
            }
            strata_pub_hub *p = strata_pub_bind(payload);
            if (!p) { proxy_send_resp(pipe_fd, -1, NULL, 0); break; }
            int h = proxy_alloc_pub(pub_handles);
            if (h < 0) {
                strata_pub_close(p);
                proxy_send_resp(pipe_fd, -1, NULL, 0);
                break;
            }
            pub_handles[h] = p;
            proxy_send_resp(pipe_fd, h, NULL, 0);
            break;
        }

        case OP_PUB_SEND: {
            if (handle >= PROXY_MAX_PUB || !pub_handles[handle]) {
                proxy_send_resp(pipe_fd, -1, NULL, 0);
                break;
            }
            if (len < 2) { proxy_send_resp(pipe_fd, -1, NULL, 0); break; }
            uint16_t net_tlen;
            memcpy(&net_tlen, payload, 2);
            uint16_t tlen = ntohs(net_tlen);
            if ((uint32_t)(2 + tlen) > len) {
                proxy_send_resp(pipe_fd, -1, NULL, 0);
                break;
            }
            int rc = strata_pub_send(pub_handles[handle],
                                     payload + 2, tlen,
                                     payload + 2 + tlen,
                                     len - 2 - tlen);
            proxy_send_resp(pipe_fd, rc, NULL, 0);
            break;
        }

        case OP_PUB_CLOSE: {
            if (handle < PROXY_MAX_PUB && pub_handles[handle]) {
                strata_pub_close(pub_handles[handle]);
                pub_handles[handle] = NULL;
            }
            proxy_send_resp(pipe_fd, 0, NULL, 0);
            break;
        }

        default:
            proxy_send_resp(pipe_fd, -1, NULL, 0);
            break;
        }

        free(payload);
    }

    /* Cleanup all open handles */
    for (int i = 0; i < PROXY_MAX_HANDLES; i++)
        if (handles[i]) strata_sock_close(handles[i]);
    for (int i = 0; i < PROXY_MAX_PUB; i++)
        if (pub_handles[i]) strata_pub_close(pub_handles[i]);

    return 0;
}
