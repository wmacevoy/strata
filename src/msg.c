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

/* ---- REQ/REP ---- */

strata_sock *strata_rep_bind(const char *endpoint) {
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
    if (!rep || !rep->is_listener) return NULL;

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
    char host[256];
    int port;
    if (strata_endpoint_parse(endpoint, host, sizeof(host), &port) != 0)
        return NULL;

    int fd = tcp_connect(host, port);
    if (fd < 0) return NULL;

    return sock_new(fd);
}

int strata_msg_send(strata_sock *sock, const void *buf, size_t len, int flags) {
    if (!sock || sock->fd < 0) return -1;
    (void)flags;
    return send_frame(sock->fd, buf, len, sock->send_ms);
}

int strata_msg_recv(strata_sock *sock, void *buf, size_t cap, int flags) {
    if (!sock || sock->fd < 0) return -1;
    (void)flags;
    return recv_frame(sock->fd, buf, cap, sock->recv_ms);
}

void strata_msg_set_timeout(strata_sock *sock, int recv_ms, int send_ms) {
    if (!sock) return;
    sock->recv_ms = recv_ms;
    sock->send_ms = send_ms;
}

void strata_sock_close(strata_sock *sock) {
    if (!sock) return;
    if (sock->fd >= 0) close(sock->fd);
    free(sock);
}

int strata_sock_fd(strata_sock *sock) {
    return sock ? sock->fd : -1;
}

/* ---- PUB/SUB ---- */

strata_pub_hub *strata_pub_bind(const char *endpoint) {
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
    for (int i = 0; i < pub->sub_count; i++)
        close(pub->subs[i].fd);
    if (pub->listen_fd >= 0) close(pub->listen_fd);
    free(pub);
}

strata_sock *strata_sub_connect(const char *endpoint) {
    char host[256];
    int port;
    if (strata_endpoint_parse(endpoint, host, sizeof(host), &port) != 0)
        return NULL;

    int fd = tcp_connect(host, port);
    if (fd < 0) return NULL;

    return sock_new(fd);
}

int strata_sub_subscribe(strata_sock *sock, const char *prefix) {
    if (!sock || sock->fd < 0) return -1;

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
    if (!sock || sock->fd < 0) return -1;

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
