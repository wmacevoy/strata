#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <zmq.h>

#include "strata/village.h"
#include "strata/agent.h"
#include "strata/json_util.h"

/* ------------------------------------------------------------------ */
/*  Local clone                                                        */
/* ------------------------------------------------------------------ */

pid_t strata_clone(strata_agent_host *host, const char *agent_name,
                   const char *event_json, int event_len) {
    return strata_agent_spawn(host, agent_name, event_json, event_len);
}

/* ------------------------------------------------------------------ */
/*  Relay                                                              */
/* ------------------------------------------------------------------ */

struct strata_relay {
    pid_t req_relay_pid;
    pid_t sub_relay_pid;
};

static void req_relay_run(const char *local_rep_ep, const char *remote_req_ep) {
    void *ctx = zmq_ctx_new();
    void *rep = zmq_socket(ctx, ZMQ_REP);
    void *req = zmq_socket(ctx, ZMQ_REQ);
    int timeout = 5000;

    zmq_bind(rep, local_rep_ep);
    zmq_connect(req, remote_req_ep);
    zmq_setsockopt(rep, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_setsockopt(req, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));

    while (1) {
        char buf[16384];
        int rc = zmq_recv(rep, buf, sizeof(buf), 0);
        if (rc < 0) continue;

        zmq_send(req, buf, rc, 0);
        rc = zmq_recv(req, buf, sizeof(buf), 0);
        if (rc < 0) {
            const char *err = "{\"ok\":false,\"error\":\"relay timeout\"}";
            zmq_send(rep, err, strlen(err), 0);
            continue;
        }
        zmq_send(rep, buf, rc, 0);
    }
}

static void sub_relay_run(const char *local_pub_ep, const char *remote_sub_ep) {
    void *ctx = zmq_ctx_new();
    void *sub = zmq_socket(ctx, ZMQ_SUB);
    void *pub = zmq_socket(ctx, ZMQ_PUB);
    int timeout = 1000;

    zmq_connect(sub, remote_sub_ep);
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, "", 0);
    zmq_setsockopt(sub, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_bind(pub, local_pub_ep);

    while (1) {
        char topic[512], payload[8192];
        int rc = zmq_recv(sub, topic, sizeof(topic), 0);
        if (rc < 0) continue;
        int tlen = rc;
        rc = zmq_recv(sub, payload, sizeof(payload), 0);
        if (rc < 0) continue;
        zmq_send(pub, topic, tlen, ZMQ_SNDMORE);
        zmq_send(pub, payload, rc, 0);
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

int strata_remote_clone(strata_agent_host *host, const char *agent_name,
                        const char *village_endpoint,
                        const char *origin_req_endpoint,
                        const char *event_json, int event_len,
                        strata_clone_result *result) {
    if (!host || !agent_name || !village_endpoint || !result) return -1;

    const strata_agent_def *def = strata_agent_host_find(host, agent_name);
    if (!def) return -1;

    void *zmq_ctx = zmq_ctx_new();
    void *req = zmq_socket(zmq_ctx, ZMQ_REQ);
    int timeout = 10000;
    zmq_setsockopt(req, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_connect(req, village_endpoint);

    /* Frame 0: JSON header */
    char header[2048];
    snprintf(header, sizeof(header),
        "{\"action\":\"clone\","
        "\"agent_name\":\"%s\","
        "\"mode\":\"%s\","
        "\"origin_req\":\"%s\"}",
        def->name,
        def->mode == STRATA_MODE_JS ? "js" : "wasm",
        origin_req_endpoint ? origin_req_endpoint : "");

    zmq_send(req, header, strlen(header), ZMQ_SNDMORE);

    /* Frame 1: agent binary */
    if (def->mode == STRATA_MODE_JS) {
        zmq_send(req, def->js_source, strlen(def->js_source), ZMQ_SNDMORE);
    } else {
        zmq_send(req, def->wasm_buf, def->wasm_len, ZMQ_SNDMORE);
    }

    /* Frame 2: event JSON */
    zmq_send(req, event_json ? event_json : "{}", event_json ? event_len : 2, 0);

    /* Receive response */
    char resp[4096];
    int rc = zmq_recv(req, resp, sizeof(resp) - 1, 0);

    zmq_close(req);
    zmq_ctx_destroy(zmq_ctx);

    if (rc < 0) {
        result->ok = 0;
        strncpy(result->error, "timeout", sizeof(result->error));
        return -1;
    }
    resp[rc] = '\0';

    memset(result, 0, sizeof(*result));
    if (strstr(resp, "\"ok\":true")) {
        result->ok = 1;
        json_get_string(resp, "agent_rep", result->agent_rep, sizeof(result->agent_rep));
        json_get_string(resp, "agent_pub", result->agent_pub, sizeof(result->agent_pub));
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
    pid_t agent_pid;
    strata_relay *relay;
    strata_agent_host *host;
} spawned_entry;

static spawned_entry spawned[MAX_SPAWNED];
static int spawned_count = 0;

static int handle_clone_request(void *rep_sock) {
    /* Frame 0: JSON header */
    char header[4096];
    int hlen = zmq_recv(rep_sock, header, sizeof(header) - 1, 0);
    if (hlen < 0) return -1;
    header[hlen] = '\0';

    int more = 0;
    size_t more_size = sizeof(more);
    zmq_getsockopt(rep_sock, ZMQ_RCVMORE, &more, &more_size);
    if (!more) {
        const char *err = "{\"ok\":false,\"error\":\"missing payload frame\"}";
        zmq_send(rep_sock, err, strlen(err), 0);
        return -1;
    }

    /* Frame 1: agent binary */
    size_t payload_cap = 2 * 1024 * 1024;
    unsigned char *payload = malloc(payload_cap);
    int plen = zmq_recv(rep_sock, payload, payload_cap, 0);
    if (plen < 0) {
        free(payload);
        const char *err = "{\"ok\":false,\"error\":\"payload recv failed\"}";
        zmq_send(rep_sock, err, strlen(err), 0);
        return -1;
    }

    /* Frame 2: event JSON */
    char event[8192] = "{}";
    int event_len = 2;
    zmq_getsockopt(rep_sock, ZMQ_RCVMORE, &more, &more_size);
    if (more) {
        event_len = zmq_recv(rep_sock, event, sizeof(event) - 1, 0);
        if (event_len < 0) event_len = 2;
        else event[event_len] = '\0';
    }

    /* Parse header */
    char agent_name[64] = {0}, mode[8] = {0}, origin_req[256] = {0};
    json_get_string(header, "agent_name", agent_name, sizeof(agent_name));
    json_get_string(header, "mode", mode, sizeof(mode));
    json_get_string(header, "origin_req", origin_req, sizeof(origin_req));

    /* Allocate local ports */
    int port = next_port;
    next_port += 4;
    char relay_rep[64], agent_rep[64], agent_pub[64];
    snprintf(relay_rep, sizeof(relay_rep), "tcp://127.0.0.1:%d", port);
    snprintf(agent_rep, sizeof(agent_rep), "tcp://127.0.0.1:%d", port + 1);
    snprintf(agent_pub, sizeof(agent_pub), "tcp://127.0.0.1:%d", port + 2);

    /* Start relay: local REP <-> origin REQ (store service) */
    strata_relay *relay = NULL;
    if (origin_req[0]) {
        relay = strata_relay_create(relay_rep, origin_req, NULL, NULL);
        usleep(50000);  /* let relay bind */
    }

    /* Register agent from received buffer */
    strata_agent_host *host = strata_agent_host_create();
    int rc;
    if (strcmp(mode, "js") == 0) {
        /* Null-terminate JS source */
        char *js = malloc(plen + 1);
        memcpy(js, payload, plen);
        js[plen] = '\0';
        rc = strata_agent_register_js_buf(host, agent_name, js,
                                           NULL, relay_rep, agent_pub, agent_rep);
        free(js);
    } else {
        rc = strata_agent_register_wasm_buf(host, agent_name,
                                             payload, plen, NULL, relay_rep);
    }
    free(payload);

    if (rc != 0) {
        const char *err = "{\"ok\":false,\"error\":\"register failed\"}";
        zmq_send(rep_sock, err, strlen(err), 0);
        strata_agent_host_free(host);
        if (relay) strata_relay_destroy(relay);
        return -1;
    }

    /* Spawn */
    pid_t pid = strata_agent_spawn(host, agent_name, event, event_len);

    char resp[512];
    if (pid > 0) {
        snprintf(resp, sizeof(resp),
            "{\"ok\":true,\"agent_name\":\"%s\","
            "\"agent_rep\":\"%s\",\"agent_pub\":\"%s\"}",
            agent_name, agent_rep, agent_pub);

        if (spawned_count < MAX_SPAWNED) {
            spawned[spawned_count].agent_pid = pid;
            spawned[spawned_count].relay = relay;
            spawned[spawned_count].host = host;
            spawned_count++;
        }
    } else {
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"spawn failed\"}");
        strata_agent_host_free(host);
        if (relay) strata_relay_destroy(relay);
    }

    zmq_send(rep_sock, resp, strlen(resp), 0);
    return pid > 0 ? 0 : -1;
}

int strata_village_run(const char *listen_endpoint) {
    signal(SIGINT, village_sigint);
    signal(SIGTERM, village_sigint);

    void *zmq_ctx = zmq_ctx_new();
    void *rep = zmq_socket(zmq_ctx, ZMQ_REP);
    zmq_bind(rep, listen_endpoint);

    int timeout = 1000;
    zmq_setsockopt(rep, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));

    fprintf(stderr, "village: listening on %s\n", listen_endpoint);

    while (village_running) {
        handle_clone_request(rep);
    }

    /* Cleanup spawned agents and relays */
    for (int i = 0; i < spawned_count; i++) {
        if (spawned[i].agent_pid > 0) {
            kill(spawned[i].agent_pid, SIGTERM);
            waitpid(spawned[i].agent_pid, NULL, 0);
        }
        if (spawned[i].relay) strata_relay_destroy(spawned[i].relay);
        if (spawned[i].host) strata_agent_host_free(spawned[i].host);
    }

    zmq_close(rep);
    zmq_ctx_destroy(zmq_ctx);
    return 0;
}

#ifndef VILLAGE_NO_MAIN
int main(int argc, char **argv) {
    const char *endpoint = argc > 1 ? argv[1] : "tcp://0.0.0.0:6000";
    return strata_village_run(endpoint);
}
#endif
