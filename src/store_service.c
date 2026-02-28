/*
 * Store service: ZMQ REP ↔ SQLite store bridge.
 *
 * Listens on a ZMQ REP socket, receives JSON requests,
 * executes them against the role-filtered store, returns JSON responses.
 *
 * Protocol (artifacts):
 *   PUT:       {"action":"put","repo":"...","type":"...","content":"...","author":"...","roles":["..."]}
 *   LIST:      {"action":"list","repo":"...","type":"...","entity":"..."}
 *   GET:       {"action":"get","id":"...","entity":"..."}
 *
 * Protocol (blobs):
 *   BLOB_PUT:  {"action":"blob_put","content":"...","entity":"...","tags":["..."],"roles":["..."]}
 *   BLOB_GET:  {"action":"blob_get","id":"...","entity":"..."}
 *   BLOB_FIND: {"action":"blob_find","entity":"...","tags":["..."]}
 *   BLOB_TAG:  {"action":"blob_tag","id":"...","tag":"..."}
 *   BLOB_UNTAG:{"action":"blob_untag","id":"...","tag":"..."}
 *   BLOB_TAGS: {"action":"blob_tags","id":"..."}
 *
 * Protocol (admin):
 *   REPO_CREATE: {"action":"repo_create","repo":"...","name":"..."}
 *   ROLE_ASSIGN: {"action":"role_assign","entity":"...","role":"...","repo":"..."}
 *   ROLE_REVOKE: {"action":"role_revoke","entity":"...","role":"...","repo":"..."}
 *   PRIVILEGE_GRANT:  {"action":"privilege_grant","entity":"...","privilege":"..."}
 *   PRIVILEGE_REVOKE: {"action":"privilege_revoke","entity":"...","privilege":"..."}
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <zmq.h>
#include "strata/store.h"
#include "strata/context.h"
#include "strata/blob.h"
#include "strata/json_util.h"

static volatile int running = 1;

static void sigint_handler(int sig) { (void)sig; running = 0; }

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
    char safe_content[8192];
    json_escape((const char *)a->content, (int)a->content_len,
                safe_content, sizeof(safe_content));

    n = snprintf(st->buf + st->pos, st->cap - st->pos,
        "{\"id\":\"%s\",\"author\":\"%s\",\"type\":\"%s\","
        "\"created_at\":\"%s\",\"content\":\"%s\"}",
        a->artifact_id, a->author, a->artifact_type,
        a->created_at, safe_content);
    if (n > 0) st->pos += n;
    st->count++;
    return 1;
}

static int blob_find_cb(const strata_blob *b, void *userdata) {
    list_state *st = userdata;
    int n;
    if (st->count > 0) {
        n = snprintf(st->buf + st->pos, st->cap - st->pos, ",");
        st->pos += n;
    }
    char safe[8192];
    json_escape((const char *)b->content, (int)b->content_len,
                safe, sizeof(safe));

    n = snprintf(st->buf + st->pos, st->cap - st->pos,
        "{\"id\":\"%s\",\"author\":\"%s\","
        "\"created_at\":\"%s\",\"content\":\"%s\"}",
        b->blob_id, b->author, b->created_at, safe);
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
            char safe[8192];
            json_escape((const char *)out.content, (int)out.content_len,
                        safe, sizeof(safe));
            snprintf(resp, resp_cap,
                "{\"ok\":true,\"id\":\"%s\",\"content\":\"%s\","
                "\"author\":\"%s\",\"type\":\"%s\",\"created_at\":\"%s\"}",
                out.artifact_id, safe, out.author, out.artifact_type, out.created_at);
            strata_artifact_cleanup(&out);
        } else {
            snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"not found or no access\"}");
        }
        strata_ctx_free(ctx);

    } else if (strcmp(action, "blob_put") == 0) {
        char content[4096] = {0}, entity[256] = {0};
        json_get_string(req, "content", content, sizeof(content));
        json_get_string(req, "entity", entity, sizeof(entity));

        char *tags[16], *roles[16];
        int ntags = json_get_string_array(req, "tags", tags, 16);
        int nroles = json_get_string_array(req, "roles", roles, 16);
        if (nroles == 0) {
            for (int i = 0; i < ntags; i++) free(tags[i]);
            snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"no roles\"}");
            return;
        }

        strata_ctx *ctx = strata_ctx_create(entity);
        char blob_id[65];
        int rc = strata_blob_put(store, ctx, content, strlen(content),
                                  (const char **)tags, ntags,
                                  (const char **)roles, nroles, blob_id);
        strata_ctx_free(ctx);
        for (int i = 0; i < ntags; i++) free(tags[i]);
        for (int i = 0; i < nroles; i++) free(roles[i]);

        if (rc == 0)
            snprintf(resp, resp_cap, "{\"ok\":true,\"id\":\"%s\"}", blob_id);
        else
            snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"blob_put failed\"}");

    } else if (strcmp(action, "blob_get") == 0) {
        char id[65] = {0}, entity[256] = {0};
        json_get_string(req, "id", id, sizeof(id));
        json_get_string(req, "entity", entity, sizeof(entity));

        strata_ctx *ctx = strata_ctx_create(entity);
        strata_blob out;
        if (strata_blob_get(store, ctx, id, &out) == 0) {
            char safe[8192];
            json_escape((const char *)out.content, (int)out.content_len,
                        safe, sizeof(safe));
            snprintf(resp, resp_cap,
                "{\"ok\":true,\"id\":\"%s\",\"content\":\"%s\","
                "\"author\":\"%s\",\"created_at\":\"%s\"}",
                out.blob_id, safe, out.author, out.created_at);
            strata_blob_cleanup(&out);
        } else {
            snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"not found or no access\"}");
        }
        strata_ctx_free(ctx);

    } else if (strcmp(action, "blob_find") == 0) {
        char entity[256] = {0};
        json_get_string(req, "entity", entity, sizeof(entity));

        char *tags[16];
        int ntags = json_get_string_array(req, "tags", tags, 16);

        strata_ctx *ctx = strata_ctx_create(entity);
        list_state st = { resp, resp_cap, 0, 0 };
        int n = snprintf(st.buf, st.cap, "{\"ok\":true,\"blobs\":[");
        st.pos = n;
        strata_blob_find(store, ctx, (const char **)tags, ntags, blob_find_cb, &st);
        snprintf(st.buf + st.pos, st.cap - st.pos, "]}");
        strata_ctx_free(ctx);
        for (int i = 0; i < ntags; i++) free(tags[i]);

    } else if (strcmp(action, "blob_tag") == 0) {
        char id[65] = {0}, tag[256] = {0};
        json_get_string(req, "id", id, sizeof(id));
        json_get_string(req, "tag", tag, sizeof(tag));

        if (strata_blob_tag(store, id, tag) == 0)
            snprintf(resp, resp_cap, "{\"ok\":true}");
        else
            snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"blob_tag failed\"}");

    } else if (strcmp(action, "blob_untag") == 0) {
        char id[65] = {0}, tag[256] = {0};
        json_get_string(req, "id", id, sizeof(id));
        json_get_string(req, "tag", tag, sizeof(tag));

        if (strata_blob_untag(store, id, tag) == 0)
            snprintf(resp, resp_cap, "{\"ok\":true}");
        else
            snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"blob_untag failed\"}");

    } else if (strcmp(action, "blob_tags") == 0) {
        char id[65] = {0};
        json_get_string(req, "id", id, sizeof(id));

        char **tags = NULL;
        int count = 0;
        if (strata_blob_tags(store, id, &tags, &count) == 0) {
            int pos = snprintf(resp, resp_cap, "{\"ok\":true,\"tags\":[");
            for (int i = 0; i < count; i++) {
                pos += snprintf(resp + pos, resp_cap - pos,
                    "%s\"%s\"", i > 0 ? "," : "", tags[i]);
            }
            snprintf(resp + pos, resp_cap - pos, "]}");
            strata_blob_free_tags(tags, count);
        } else {
            snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"blob_tags failed\"}");
        }

    } else if (strcmp(action, "repo_create") == 0) {
        char repo[256] = {0}, name[256] = {0};
        json_get_string(req, "repo", repo, sizeof(repo));
        json_get_string(req, "name", name, sizeof(name));

        if (strata_repo_create(store, repo, name) == 0)
            snprintf(resp, resp_cap, "{\"ok\":true,\"repo\":\"%s\"}", repo);
        else
            snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"repo_create failed\"}");

    } else if (strcmp(action, "role_assign") == 0) {
        char entity[256] = {0}, role[256] = {0}, repo[256] = {0};
        json_get_string(req, "entity", entity, sizeof(entity));
        json_get_string(req, "role", role, sizeof(role));
        json_get_string(req, "repo", repo, sizeof(repo));

        if (strata_role_assign(store, entity, role, repo) == 0)
            snprintf(resp, resp_cap, "{\"ok\":true}");
        else
            snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"role_assign failed\"}");

    } else if (strcmp(action, "role_revoke") == 0) {
        char entity[256] = {0}, role[256] = {0}, repo[256] = {0};
        json_get_string(req, "entity", entity, sizeof(entity));
        json_get_string(req, "role", role, sizeof(role));
        json_get_string(req, "repo", repo, sizeof(repo));

        if (strata_role_revoke(store, entity, role, repo) == 0)
            snprintf(resp, resp_cap, "{\"ok\":true}");
        else
            snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"role_revoke failed\"}");

    } else if (strcmp(action, "privilege_grant") == 0) {
        char entity[256] = {0}, privilege[256] = {0};
        json_get_string(req, "entity", entity, sizeof(entity));
        json_get_string(req, "privilege", privilege, sizeof(privilege));

        if (strata_role_assign(store, entity, privilege, "_system") == 0)
            snprintf(resp, resp_cap, "{\"ok\":true}");
        else
            snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"privilege_grant failed\"}");

    } else if (strcmp(action, "privilege_revoke") == 0) {
        char entity[256] = {0}, privilege[256] = {0};
        json_get_string(req, "entity", entity, sizeof(entity));
        json_get_string(req, "privilege", privilege, sizeof(privilege));

        if (strata_role_revoke(store, entity, privilege, "_system") == 0)
            snprintf(resp, resp_cap, "{\"ok\":true}");
        else
            snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"privilege_revoke failed\"}");

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
