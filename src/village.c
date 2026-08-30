#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include "strata/transport.h"

#include "strata/village.h"
#include "strata/den.h"
#include "strata/store.h"
#include "strata/json_util.h"

/* ------------------------------------------------------------------ */
/*  Local clone                                                        */
/* ------------------------------------------------------------------ */

pid_t strata_clone(strata_den_host *host, const char *den_name,
                   const char *event_json, int event_len) {
    return strata_den_spawn(host, den_name, event_json, event_len);
}

/* ------------------------------------------------------------------ */
/*  Relay                                                              */
/* ------------------------------------------------------------------ */

struct strata_relay {
    pid_t req_relay_pid;
    pid_t sub_relay_pid;
};

static void req_relay_run(const char *local_rep_ep, const char *remote_req_ep) {
    strata_sock rep; strata_rep_open(&rep);
    strata_sock req; strata_req_open(&req);
    int timeout = 5000;

    strata_sock_bind(rep, local_rep_ep);
    strata_sock_dial(req, remote_req_ep);
    strata_sock_set_recv_timeout(rep, timeout);
    strata_sock_set_recv_timeout(req, timeout);

    while (1) {
        char buf[16384];
        int rc = strata_raw_recv(rep, buf, sizeof(buf));
        if (rc < 0) continue;

        strata_raw_send(req, buf, rc);
        rc = strata_raw_recv(req, buf, sizeof(buf));
        if (rc < 0) {
            const char *err = "{\"ok\":false,\"error\":\"relay timeout\"}";
            strata_raw_send(rep, err, strlen(err));
            continue;
        }
        strata_raw_send(rep, buf, rc);
    }
}

static void sub_relay_run(const char *local_pub_ep, const char *remote_sub_ep) {
    strata_sock sub; strata_req_open(&sub);
    strata_sock pub; strata_rep_open(&pub);
    int timeout = 1000;

    strata_sock_dial(sub, remote_sub_ep);
    strata_sock_subscribe(sub, "", 0);
    strata_sock_set_recv_timeout(sub, timeout);
    strata_sock_bind(pub, local_pub_ep);

    while (1) {
        char topic[512], payload[8192];
        int rc = strata_sub_recv(sub, topic, sizeof(topic), payload, sizeof(payload));
        if (rc < 0) continue;
        strata_pub_send(pub, topic, payload);
    }
}

strata_relay *strata_relay_create(const char *local_rep_ep,
                                   const char *remote_req_ep,
                                   const char *local_pub_ep,
                                   const char *remote_sub_ep) {
    strata_relay *relay = calloc(1, sizeof(*relay));
    if (!relay) return NULL;

    relay->req_relay_pid = fork();
    if (relay->req_relay_pid == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        req_relay_run(local_rep_ep, remote_req_ep);
        _exit(0);
    }

    if (local_pub_ep && remote_sub_ep &&
        local_pub_ep[0] && remote_sub_ep[0]) {
        relay->sub_relay_pid = fork();
        if (relay->sub_relay_pid == 0) {
            signal(SIGTERM, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            sub_relay_run(local_pub_ep, remote_sub_ep);
            _exit(0);
        }
    }

    return relay;
}

void strata_relay_destroy(strata_relay *relay) {
    if (!relay) return;
    if (relay->req_relay_pid > 0) {
        kill(relay->req_relay_pid, SIGTERM);
        waitpid(relay->req_relay_pid, NULL, 0);
    }
    if (relay->sub_relay_pid > 0) {
        kill(relay->sub_relay_pid, SIGTERM);
        waitpid(relay->sub_relay_pid, NULL, 0);
    }
    free(relay);
}

/* ------------------------------------------------------------------ */
/*  Remote clone (caller side)                                         */
/* ------------------------------------------------------------------ */

int strata_remote_clone(strata_den_host *host, const char *den_name,
                        const char *village_endpoint,
                        const char *origin_req_endpoint,
                        const char *event_json, int event_len,
                        strata_clone_result *result) {
    if (!host || !den_name || !village_endpoint || !result) return -1;

    const strata_den_def *def = strata_den_host_find(host, den_name);
    if (!def) return -1;

    /* Privilege check: den needs "parent" to be cloned remotely */
    strata_store *store = strata_den_host_get_store(host);
    if (store && def->den_id[0]) {
        if (!strata_has_privilege(store, def->den_id, "parent")) {
            result->ok = 0;
            strncpy(result->error, "no parent privilege", sizeof(result->error));
            return -1;
        }
    }

    strata_sock req; strata_req_open(&req);

    int timeout = 10000;
    strata_sock_set_recv_timeout(req, timeout);
    strata_sock_dial(req, village_endpoint);

    /* Frame 0: JSON header */
    char header[2048];
    snprintf(header, sizeof(header),
        "{\"action\":\"clone\","
        "\"den_name\":\"%s\","
        "\"mode\":\"%s\","
        "\"origin_req\":\"%s\"}",
        def->name,
        def->mode == STRATA_MODE_JS ? "js" : "native",
        origin_req_endpoint ? origin_req_endpoint : "");

    /* Send 3 frames: header + source + event */
    const char *parts[3];
    size_t lens[3];

    parts[0] = header;
    lens[0] = strlen(header);

    if (def->mode == STRATA_MODE_JS) {
        parts[1] = def->js_source;
        lens[1] = strlen(def->js_source);
    } else {
        parts[1] = def->c_source;
        lens[1] = def->c_source_len;
    }

    const char *ev = event_json ? event_json : "{}";
    int ev_len = event_json ? event_len : 2;
    parts[2] = ev;
    lens[2] = ev_len;

    strata_multipart_send(req, parts, lens, 3);

    /* Receive response */
    char resp[4096];
    int rc = strata_raw_recv(req, resp, sizeof(resp) - 1);

    strata_sock_close(req);

    if (rc < 0) {
        result->ok = 0;
        strncpy(result->error, "timeout", sizeof(result->error));
        return -1;
    }
    resp[rc] = '\0';

    memset(result, 0, sizeof(*result));
    if (strstr(resp, "\"ok\":true")) {
        result->ok = 1;
        json_get_string(resp, "den_rep", result->den_rep, sizeof(result->den_rep));
        json_get_string(resp, "den_pub", result->den_pub, sizeof(result->den_pub));
    } else {
        result->ok = 0;
        json_get_string(resp, "error", result->error, sizeof(result->error));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Village daemon                                                     */
/* ------------------------------------------------------------------ */

static volatile int village_running = 1;

static void village_sigint(int sig) { (void)sig; village_running = 0; }

static int next_port = 16000;

#define MAX_SPAWNED 64

typedef struct {
    pid_t den_pid;
    strata_relay *relay;
    strata_den_host *host;
} spawned_entry;

static spawned_entry spawned[MAX_SPAWNED];
static int spawned_count = 0;

static int handle_clone_request(strata_sock rep_sock) {
    /* Receive up to 3 frames via multipart */
    size_t payload_cap = 2 * 1024 * 1024;
    unsigned char *payload = malloc(payload_cap);
    char header[4096];
    char event[8192] = "{}";

    char *bufs[3] = { header, (char *)payload, event };
    size_t caps[3] = { sizeof(header) - 1, payload_cap, sizeof(event) - 1 };
    size_t lens[3] = { 0, 0, 0 };

    int nframes = strata_multipart_recv(rep_sock, bufs, lens, caps, 3);
    if (nframes < 1) {
        free(payload);
        return -1;
    }
    header[lens[0]] = '\0';

    if (nframes < 2) {
        free(payload);
        const char *err = "{\"ok\":false,\"error\":\"missing payload frame\"}";
        strata_raw_send(rep_sock, err, strlen(err));
        return -1;
    }

    (void)lens[0]; /* hlen consumed via null-termination above */
    int plen = (int)lens[1];

    /* Frame 2: event JSON */
    int event_len = 2;
    if (nframes >= 3 && lens[2] > 0) {
        event_len = (int)lens[2];
        event[event_len] = '\0';
    }

    /* Parse header */
    char den_name[64] = {0}, mode[8] = {0}, origin_req[256] = {0};
    json_get_string(header, "den_name", den_name, sizeof(den_name));
    json_get_string(header, "mode", mode, sizeof(mode));
    json_get_string(header, "origin_req", origin_req, sizeof(origin_req));

    /* Allocate local ports */
    int port = next_port;
    next_port += 4;
    char relay_rep[64], den_rep[64], den_pub[64];
    snprintf(relay_rep, sizeof(relay_rep), "tcp://127.0.0.1:%d", port);
    snprintf(den_rep, sizeof(den_rep), "tcp://127.0.0.1:%d", port + 1);
    snprintf(den_pub, sizeof(den_pub), "tcp://127.0.0.1:%d", port + 2);

    /* Start relay: local REP <-> origin REQ (store service) */
    strata_relay *relay = NULL;
    if (origin_req[0]) {
        relay = strata_relay_create(relay_rep, origin_req, NULL, NULL);
        usleep(50000);  /* let relay bind */
    }

    /* Register den from received buffer */
    strata_den_host *host = strata_den_host_create();
    int rc;
    if (strcmp(mode, "js") == 0) {
        /* Null-terminate JS source */
        char *js = malloc(plen + 1);
        memcpy(js, payload, plen);
        js[plen] = '\0';
        rc = strata_den_register_js_buf(host, den_name, js,
                                           NULL, relay_rep, den_pub, den_rep);
        free(js);
    } else {
        /* Null-terminate C source */
        char *csrc = malloc(plen + 1);
        memcpy(csrc, payload, plen);
        csrc[plen] = '\0';
        rc = strata_den_register_native_buf(host, den_name,
                                               csrc, plen, NULL, relay_rep);
        free(csrc);
    }
    free(payload);

    if (rc != 0) {
        const char *err = "{\"ok\":false,\"error\":\"register failed\"}";
        strata_raw_send(rep_sock, err, strlen(err));
        strata_den_host_free(host);
        if (relay) strata_relay_destroy(relay);
        return -1;
    }

    /* Spawn */
    pid_t pid = strata_den_spawn(host, den_name, event, event_len);

    char resp[512];
    if (pid > 0) {
        snprintf(resp, sizeof(resp),
            "{\"ok\":true,\"den_name\":\"%s\","
            "\"den_rep\":\"%s\",\"den_pub\":\"%s\"}",
            den_name, den_rep, den_pub);

        if (spawned_count < MAX_SPAWNED) {
            spawned[spawned_count].den_pid = pid;
            spawned[spawned_count].relay = relay;
            spawned[spawned_count].host = host;
            spawned_count++;
        }
    } else {
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"spawn failed\"}");
        strata_den_host_free(host);
        if (relay) strata_relay_destroy(relay);
    }

    strata_raw_send(rep_sock, resp, strlen(resp));
    return pid > 0 ? 0 : -1;
}

int strata_village_run(const char *listen_endpoint) {
    signal(SIGINT, village_sigint);
    signal(SIGTERM, village_sigint);

    strata_sock rep; strata_rep_open(&rep);

    strata_sock_bind(rep, listen_endpoint);

    int timeout = 1000;
    strata_sock_set_recv_timeout(rep, timeout);

    fprintf(stderr, "village: listening on %s\n", listen_endpoint);

    while (village_running) {
        handle_clone_request(rep);
    }

    /* Cleanup spawned dens and relays */
    for (int i = 0; i < spawned_count; i++) {
        if (spawned[i].den_pid > 0) {
            kill(spawned[i].den_pid, SIGTERM);
            waitpid(spawned[i].den_pid, NULL, 0);
        }
        if (spawned[i].relay) strata_relay_destroy(spawned[i].relay);
        if (spawned[i].host) strata_den_host_free(spawned[i].host);
    }

    strata_sock_close(rep);
    return 0;
}

#ifndef VILLAGE_NO_MAIN
int main(int argc, char **argv) {
    const char *endpoint = argc > 1 ? argv[1] : "tcp://0.0.0.0:6000";
    return strata_village_run(endpoint);
}
#endif
