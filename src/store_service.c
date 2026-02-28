/*
 * Store service: ZMQ REP ↔ SQLite store bridge.
 *
 * Listens on a ZMQ REP socket, receives JSON requests,
 * executes them against the role-filtered store, returns JSON responses.
 *
 * Protocol:
 *   PUT:  {"action":"put","repo":"...","type":"...","content":"...","author":"...","roles":["..."]}
 *   LIST: {"action":"list","repo":"...","type":"...","entity":"..."}
 *   GET:  {"action":"get","id":"...","entity":"..."}
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <zmq.h>
#include "strata/store.h"
#include "strata/context.h"

static volatile int running = 1;

static void sigint_handler(int sig) { (void)sig; running = 0; }

/* Minimal JSON field extraction — finds "key":"value" and copies value */
static int json_get_string(const char *json, const char *key,
                           char *out, int out_cap) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *start = strstr(json, pattern);
    if (!start) return -1;
    start += strlen(pattern);
    const char *end = strchr(start, '"');
    if (!end) return -1;
    int len = (int)(end - start);
    if (len >= out_cap) len = out_cap - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return len;
}

/* Extract a JSON array of strings: "key":["a","b"] */
static int json_get_string_array(const char *json, const char *key,
                                 char **out, int max_items) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":[", key);
    const char *start = strstr(json, pattern);
    if (!start) return 0;
    start += strlen(pattern);
    const char *end = strchr(start, ']');
    if (!end) return 0;

    int count = 0;
    const char *p = start;
    while (p < end && count < max_items) {
        const char *q = strchr(p, '"');
        if (!q || q >= end) break;
        q++;
        const char *r = strchr(q, '"');
        if (!r || r >= end) break;
        int len = (int)(r - q);
        out[count] = malloc(len + 1);
        memcpy(out[count], q, len);
        out[count][len] = '\0';
        count++;
        p = r + 1;
    }
    return count;
}

/* List callback state */
typedef struct {
    char *buf;
    int cap;
    int pos;
    int count;
} list_state;

static int list_cb(const strata_artifact *a, void *userdata) {
    list_state *st = userdata;
    int n;
    if (st->count > 0) {
        n = snprintf(st->buf + st->pos, st->cap - st->pos, ",");
        st->pos += n;
    }
    /* Escape content for JSON (simple: just truncate at first quote) */
    char safe_content[4096];
    int clen = (int)(a->content_len < sizeof(safe_content) - 1 ? a->content_len : sizeof(safe_content) - 1);
    memcpy(safe_content, a->content, clen);
    safe_content[clen] = '\0';

    n = snprintf(st->buf + st->pos, st->cap - st->pos,
        "{\"id\":\"%s\",\"author\":\"%s\",\"type\":\"%s\","
        "\"created_at\":\"%s\",\"content\":\"%s\"}",
        a->artifact_id, a->author, a->artifact_type,
        a->created_at, safe_content);
    if (n > 0) st->pos += n;
    st->count++;
    return 1;
}

static void handle_request(strata_store *store, const char *req, int req_len,
                           char *resp, int resp_cap) {
    char action[32] = {0};
    json_get_string(req, "action", action, sizeof(action));

    if (strcmp(action, "put") == 0) {
        char repo[256] = {0}, type[64] = {0}, content[4096] = {0}, author[256] = {0};
        json_get_string(req, "repo", repo, sizeof(repo));
        json_get_string(req, "type", type, sizeof(type));
        json_get_string(req, "content", content, sizeof(content));
        json_get_string(req, "author", author, sizeof(author));

        char *roles[16];
        int nroles = json_get_string_array(req, "roles", roles, 16);
        if (nroles == 0) {
            snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"no roles\"}");
            return;
        }

        strata_ctx *ctx = strata_ctx_create(author);
        char artifact_id[65];
        int rc = strata_artifact_put(store, ctx, repo, type,
                                     content, strlen(content),
                                     (const char **)roles, nroles, artifact_id);
        strata_ctx_free(ctx);
        for (int i = 0; i < nroles; i++) free(roles[i]);

        if (rc == 0)
            snprintf(resp, resp_cap, "{\"ok\":true,\"id\":\"%s\"}", artifact_id);
        else
            snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"put failed\"}");

    } else if (strcmp(action, "list") == 0) {
        char repo[256] = {0}, type[64] = {0}, entity[256] = {0};
        json_get_string(req, "repo", repo, sizeof(repo));
        json_get_string(req, "type", type, sizeof(type));
        json_get_string(req, "entity", entity, sizeof(entity));

        strata_ctx *ctx = strata_ctx_create(entity);
        list_state st = { resp, resp_cap, 0, 0 };
        int n = snprintf(st.buf, st.cap, "{\"ok\":true,\"artifacts\":[");
        st.pos = n;
        strata_artifact_list(store, ctx, repo, type[0] ? type : NULL, list_cb, &st);
        snprintf(st.buf + st.pos, st.cap - st.pos, "]}");
        strata_ctx_free(ctx);

    } else if (strcmp(action, "get") == 0) {
        char id[65] = {0}, entity[256] = {0};
        json_get_string(req, "id", id, sizeof(id));
        json_get_string(req, "entity", entity, sizeof(entity));

        strata_ctx *ctx = strata_ctx_create(entity);
        strata_artifact out;
        if (strata_artifact_get(store, ctx, id, &out) == 0) {
            char safe[4096];
            int clen = (int)(out.content_len < sizeof(safe) - 1 ? out.content_len : sizeof(safe) - 1);
            memcpy(safe, out.content, clen);
            safe[clen] = '\0';
            snprintf(resp, resp_cap,
                "{\"ok\":true,\"id\":\"%s\",\"content\":\"%s\","
                "\"author\":\"%s\",\"type\":\"%s\",\"created_at\":\"%s\"}",
                out.artifact_id, safe, out.author, out.artifact_type, out.created_at);
            strata_artifact_cleanup(&out);
        } else {
            snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"not found or no access\"}");
        }
        strata_ctx_free(ctx);

    } else {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"unknown action\"}");
    }
}

int store_service_run(const char *db_path, const char *endpoint) {
    signal(SIGINT, sigint_handler);

    strata_store *store = strata_store_open_sqlite(db_path);
    if (!store) { fprintf(stderr, "failed to open store\n"); return 1; }
    strata_store_init(store);

    void *zmq_ctx = zmq_ctx_new();
    void *rep = zmq_socket(zmq_ctx, ZMQ_REP);
    zmq_bind(rep, endpoint);

    int timeout = 1000;
    zmq_setsockopt(rep, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));

    fprintf(stderr, "store_service: listening on %s\n", endpoint);

    while (running) {
        char req[8192] = {0};
        int rc = zmq_recv(rep, req, sizeof(req) - 1, 0);
        if (rc < 0) continue;
        req[rc] = '\0';

        char resp[16384] = {0};
        handle_request(store, req, rc, resp, sizeof(resp));
        zmq_send(rep, resp, strlen(resp), 0);
    }

    zmq_close(rep);
    zmq_ctx_destroy(zmq_ctx);
    strata_store_close(store);
    return 0;
}

/* Can be used as a standalone main or called from tests */
#ifndef STORE_SERVICE_NO_MAIN
int main(int argc, char **argv) {
    const char *db_path = argc > 1 ? argv[1] : "/tmp/strata_store.db";
    const char *endpoint = argc > 2 ? argv[2] : "tcp://127.0.0.1:5560";
    return store_service_run(db_path, endpoint);
}
#endif
