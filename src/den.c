#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sqlite3.h>
#include <libtcc.h>

#include "strata/den.h"
#include "strata/store.h"
#include "strata/sandbox.h"
#include "strata/msg.h"
#include "strata/aead.h"

/* ------------------------------------------------------------------ */
/*  Bedrock context — shared by native C and QuickJS bindings        */
/* ------------------------------------------------------------------ */

typedef struct {
    strata_sock *sub_sock;
    strata_pub_hub *pub_hub;
    strata_sock *rep_listener;
    strata_sock *rep_client;    /* current client between serve_recv/serve_send */
    char req_endpoint[256];     /* for per-request REQ connections */
    sqlite3 *local_db;
    char den_name[64];
    char den_entity[256];
    char local_db_path[256];
} bedrock_ctx_t;

/* ------------------------------------------------------------------ */
/*  Socket setup helpers                                               */
/* ------------------------------------------------------------------ */

static void bedrock_setup(bedrock_ctx_t *bedrock, strata_den_def *def) {
    memset(bedrock, 0, sizeof(*bedrock));

    if (def->sub_endpoint[0]) {
        bedrock->sub_sock = strata_sub_connect(def->sub_endpoint);
        if (bedrock->sub_sock)
            strata_msg_set_timeout(bedrock->sub_sock, 5000, -1);
    }
    if (def->req_endpoint[0]) {
        snprintf(bedrock->req_endpoint, sizeof(bedrock->req_endpoint),
                 "%s", def->req_endpoint);
    }
    if (def->pub_endpoint[0]) {
        bedrock->pub_hub = strata_pub_bind(def->pub_endpoint);
    }
    if (def->rep_endpoint[0]) {
        bedrock->rep_listener = strata_rep_bind(def->rep_endpoint);
        if (bedrock->rep_listener)
            strata_msg_set_timeout(bedrock->rep_listener, 5000, -1);
    }
}

static void bedrock_teardown(bedrock_ctx_t *bedrock) {
    if (bedrock->local_db) sqlite3_close(bedrock->local_db);
    if (bedrock->rep_client)  strata_sock_close(bedrock->rep_client);
    if (bedrock->sub_sock)    strata_sock_close(bedrock->sub_sock);
    if (bedrock->pub_hub)     strata_pub_close(bedrock->pub_hub);
    if (bedrock->rep_listener) strata_sock_close(bedrock->rep_listener);
}

/* ------------------------------------------------------------------ */
/*  Base64 encode/decode for db transport                              */
/* ------------------------------------------------------------------ */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const unsigned char *data, size_t len, size_t *out_len) {
    size_t olen = 4 * ((len + 2) / 3);
    char *out = malloc(olen + 1);
    if (!out) return NULL;
    size_t i, j;
    for (i = 0, j = 0; i + 2 < len; i += 3, j += 4) {
        unsigned int v = ((unsigned int)data[i] << 16) | (data[i+1] << 8) | data[i+2];
        out[j]   = b64_table[(v >> 18) & 0x3F];
        out[j+1] = b64_table[(v >> 12) & 0x3F];
        out[j+2] = b64_table[(v >> 6)  & 0x3F];
        out[j+3] = b64_table[v & 0x3F];
    }
    if (i < len) {
        unsigned int v = (unsigned int)data[i] << 16;
        if (i + 1 < len) v |= data[i+1] << 8;
        out[j]   = b64_table[(v >> 18) & 0x3F];
        out[j+1] = b64_table[(v >> 12) & 0x3F];
        out[j+2] = (i + 1 < len) ? b64_table[(v >> 6) & 0x3F] : '=';
        out[j+3] = '=';
        j += 4;
    }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static unsigned char *base64_decode(const char *str, size_t *out_len) {
    size_t slen = strlen(str);
    size_t olen = slen / 4 * 3;
    if (slen > 0 && str[slen-1] == '=') olen--;
    if (slen > 1 && str[slen-2] == '=') olen--;
    unsigned char *out = malloc(olen);
    if (!out) return NULL;
    size_t i, j;
    for (i = 0, j = 0; i + 3 < slen; i += 4) {
        int a = b64_val(str[i]), b = b64_val(str[i+1]);
        int c = b64_val(str[i+2]), d = b64_val(str[i+3]);
        if (a < 0 || b < 0) break;
        unsigned int v = (a << 18) | (b << 12);
        if (c >= 0) v |= (c << 6);
        if (d >= 0) v |= d;
        out[j++] = (v >> 16) & 0xFF;
        if (c >= 0 && j < olen) out[j++] = (v >> 8) & 0xFF;
        if (d >= 0 && j < olen) out[j++] = v & 0xFF;
    }
    *out_len = j;
    return out;
}

/* ------------------------------------------------------------------ */
/*  Local DB lifecycle — load from village, save back                   */
/* ------------------------------------------------------------------ */

static void local_db_load(bedrock_ctx_t *bedrock, strata_den_def *def) {
    snprintf(bedrock->den_name, sizeof(bedrock->den_name), "%s", def->name);
    snprintf(bedrock->den_entity, sizeof(bedrock->den_entity), "%s",
             def->den_id[0] ? def->den_id : def->name);
    snprintf(bedrock->local_db_path, sizeof(bedrock->local_db_path),
             "/tmp/strata_den_%s.db", def->name);

    /* Try to fetch saved db from village store */
    if (bedrock->req_endpoint[0]) {
        char req[512];
        snprintf(req, sizeof(req),
            "{\"action\":\"blob_find\",\"entity\":\"%s\",\"tags\":[\"den:%s:db\"]}",
            bedrock->den_entity, bedrock->den_name);

        strata_sock *sock = strata_req_connect(bedrock->req_endpoint);
        if (sock) {
            strata_msg_set_timeout(sock, 30000, 5000);
            int rc = strata_send(sock, req, strlen(req), 0);
            if (rc >= 0) {
                /* Use large buffer for db content */
                size_t resp_cap = 4 * 1024 * 1024;  /* 4 MB */
                char *resp = malloc(resp_cap);
                if (resp) {
                    rc = strata_recv(sock, resp, resp_cap - 1, 0);

                    if (rc > 0) {
                        resp[rc] = '\0';
                        /* Extract base64 content from last blob in response */
                        /* Response format: {"ok":true,"blobs":[{"content":"base64..."}]} */
                        char *content_key = strstr(resp, "\"content\":\"");
                        if (content_key) {
                            /* Find the LAST content field (latest blob) */
                            char *last_content = content_key;
                            while ((content_key = strstr(content_key + 1, "\"content\":\"")) != NULL)
                                last_content = content_key;

                            char *b64_start = last_content + 11;  /* skip "content":" */
                            char *b64_end = strchr(b64_start, '"');
                            if (b64_end) {
                                *b64_end = '\0';
                                size_t db_len = 0;
                                unsigned char *db_data = base64_decode(b64_start, &db_len);
                                if (db_data && db_len > 0) {
                                    FILE *f = fopen(bedrock->local_db_path, "wb");
                                    if (f) {
                                        fwrite(db_data, 1, db_len, f);
                                        fclose(f);
                                        fprintf(stderr, "[%s] restored db (%zu bytes)\n",
                                                bedrock->den_name, db_len);
                                    }
                                    free(db_data);
                                }
                            }
                        }
                    }
                    free(resp);
                }
            }
            strata_sock_close(sock);
        }
    }

    /* Open local db (creates fresh if file doesn't exist) */
    int rc = sqlite3_open(bedrock->local_db_path, &bedrock->local_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[%s] sqlite3_open failed: %s\n",
                bedrock->den_name, sqlite3_errmsg(bedrock->local_db));
        bedrock->local_db = NULL;
    } else {
        sqlite3_exec(bedrock->local_db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    }
}

static void local_db_save(bedrock_ctx_t *bedrock) {
    if (!bedrock->local_db) return;

    sqlite3_close(bedrock->local_db);
    bedrock->local_db = NULL;

    if (!bedrock->req_endpoint[0]) goto cleanup_file;

    /* Read the db file */
    FILE *f = fopen(bedrock->local_db_path, "rb");
    if (!f) goto cleanup_file;
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen <= 0) { fclose(f); goto cleanup_file; }

    unsigned char *db_data = malloc(flen);
    if (!db_data) { fclose(f); goto cleanup_file; }
    if ((long)fread(db_data, 1, flen, f) != flen) {
        free(db_data); fclose(f); goto cleanup_file;
    }
    fclose(f);

    /* Base64 encode */
    size_t b64_len = 0;
    char *b64 = base64_encode(db_data, flen, &b64_len);
    free(db_data);
    if (!b64) goto cleanup_file;

    /* Build request — need room for JSON wrapper + base64 content */
    size_t req_cap = b64_len + 512;
    char *req = malloc(req_cap);
    if (req) {
        snprintf(req, req_cap,
            "{\"action\":\"blob_put\",\"content\":\"%s\","
            "\"entity\":\"%s\",\"tags\":[\"den:%s:db\"],\"roles\":[\"owner\"]}",
            b64, bedrock->den_entity, bedrock->den_name);

        strata_sock *sock = strata_req_connect(bedrock->req_endpoint);
        if (sock) {
            strata_msg_set_timeout(sock, 30000, 5000);
            int rc = strata_send(sock, req, strlen(req), 0);
            if (rc >= 0) {
                char resp[1024];
                strata_recv(sock, resp, sizeof(resp) - 1, 0);
                fprintf(stderr, "[%s] saved db (%ld bytes)\n", bedrock->den_name, flen);
            }
            strata_sock_close(sock);
        }
        free(req);
    }
    free(b64);

cleanup_file:
    unlink(bedrock->local_db_path);
    /* Also clean WAL/SHM */
    char wal[260], shm[260];
    snprintf(wal, sizeof(wal), "%s-wal", bedrock->local_db_path);
    snprintf(shm, sizeof(shm), "%s-shm", bedrock->local_db_path);
    unlink(wal);
    unlink(shm);
}

/* ------------------------------------------------------------------ */
/*  Native C bedrock functions (used by TCC-compiled dens)             */
/* ------------------------------------------------------------------ */

/* File-scoped context — safe because each den runs in its own forked process */
static bedrock_ctx_t *g_bedrock_ctx;

static void native_bedrock_log(const char *msg) {
    fprintf(stderr, "[native] %s\n", msg ? msg : "(null)");
}

static int native_bedrock_subscribe(const char *filter) {
    if (!g_bedrock_ctx || !g_bedrock_ctx->sub_sock || !filter) return -1;
    return strata_sub_subscribe(g_bedrock_ctx->sub_sock, filter);
}

static int native_bedrock_receive(char *topic_buf, int topic_cap,
                                  char *payload_buf, int payload_cap) {
    if (!g_bedrock_ctx || !g_bedrock_ctx->sub_sock) return -1;
    return strata_sub_recv(g_bedrock_ctx->sub_sock,
                           topic_buf, topic_cap,
                           payload_buf, payload_cap);
}

static int native_bedrock_request(const char *req_json,
                                  char *resp_buf, int resp_cap) {
    if (!g_bedrock_ctx || !g_bedrock_ctx->req_endpoint[0] || !req_json) return -1;
    strata_sock *sock = strata_req_connect(g_bedrock_ctx->req_endpoint);
    if (!sock) return -1;
    strata_msg_set_timeout(sock, 5000, 5000);
    int rc = strata_send(sock, req_json, strlen(req_json), 0);
    if (rc < 0) { strata_sock_close(sock); return -1; }
    rc = strata_recv(sock, resp_buf, resp_cap - 1, 0);
    if (rc >= 0) resp_buf[rc] = '\0';
    strata_sock_close(sock);
    return rc;
}

static int native_bedrock_request_to(const char *endpoint,
                                     const char *req_json,
                                     char *resp_buf, int resp_cap) {
    if (!g_bedrock_ctx || !endpoint || !req_json) return -1;
    strata_sock *sock = strata_req_connect(endpoint);
    if (!sock) return -1;
    strata_msg_set_timeout(sock, 60000, 60000);
    int rc = strata_send(sock, req_json, strlen(req_json), 0);
    if (rc < 0) { strata_sock_close(sock); return -1; }
    rc = strata_recv(sock, resp_buf, resp_cap - 1, 0);
    if (rc >= 0) resp_buf[rc] = '\0';
    strata_sock_close(sock);
    return rc;
}

static int native_bedrock_publish(const char *topic, const char *payload) {
    if (!g_bedrock_ctx || !g_bedrock_ctx->pub_hub || !topic || !payload) return -1;
    return strata_pub_send(g_bedrock_ctx->pub_hub,
                           topic, strlen(topic),
                           payload, strlen(payload));
}

static int native_bedrock_serve_recv(char *buf, int cap) {
    if (!g_bedrock_ctx || !g_bedrock_ctx->rep_listener) return -1;
    /* Accept a new client connection */
    strata_sock *client = strata_rep_accept(g_bedrock_ctx->rep_listener);
    if (!client) return -1;
    int rc = strata_recv(client, buf, cap - 1, 0);
    if (rc < 0) {
        strata_sock_close(client);
        return -1;
    }
    buf[rc] = '\0';
    /* Store client for the subsequent serve_send */
    g_bedrock_ctx->rep_client = client;
    return rc;
}

static int native_bedrock_serve_send(const char *resp) {
    if (!g_bedrock_ctx || !g_bedrock_ctx->rep_client || !resp) return -1;
    int rc = strata_send(g_bedrock_ctx->rep_client, resp, strlen(resp), 0);
    strata_sock_close(g_bedrock_ctx->rep_client);
    g_bedrock_ctx->rep_client = NULL;
    return rc;
}

static int native_bedrock_db_exec(const char *sql) {
    if (!g_bedrock_ctx || !g_bedrock_ctx->local_db || !sql) return -1;
    char *errmsg = NULL;
    int rc = sqlite3_exec(g_bedrock_ctx->local_db, sql, NULL, NULL, &errmsg);
    if (errmsg) {
        fprintf(stderr, "[native] db_exec: %s\n", errmsg);
        sqlite3_free(errmsg);
    }
    if (rc != SQLITE_OK) return -1;
    return sqlite3_changes(g_bedrock_ctx->local_db);
}

static int native_bedrock_db_query(const char *sql,
                                   char *result_buf, int result_cap) {
    if (!g_bedrock_ctx || !g_bedrock_ctx->local_db || !sql) return -1;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(g_bedrock_ctx->local_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    int pos = 0;
    pos += snprintf(result_buf + pos, result_cap - pos, "[");
    int first_row = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW && pos < result_cap - 2) {
        if (!first_row) pos += snprintf(result_buf + pos, result_cap - pos, ",");
        first_row = 0;
        pos += snprintf(result_buf + pos, result_cap - pos, "{");
        int ncols = sqlite3_column_count(stmt);
        for (int i = 0; i < ncols && pos < result_cap - 2; i++) {
            if (i > 0) pos += snprintf(result_buf + pos, result_cap - pos, ",");
            const char *cn = sqlite3_column_name(stmt, i);
            int type = sqlite3_column_type(stmt, i);
            if (type == SQLITE_INTEGER)
                pos += snprintf(result_buf + pos, result_cap - pos,
                    "\"%s\":%lld", cn, sqlite3_column_int64(stmt, i));
            else if (type == SQLITE_FLOAT)
                pos += snprintf(result_buf + pos, result_cap - pos,
                    "\"%s\":%g", cn, sqlite3_column_double(stmt, i));
            else if (type == SQLITE_NULL)
                pos += snprintf(result_buf + pos, result_cap - pos, "\"%s\":null", cn);
            else
                pos += snprintf(result_buf + pos, result_cap - pos,
                    "\"%s\":\"%s\"", cn, (const char *)sqlite3_column_text(stmt, i));
        }
        pos += snprintf(result_buf + pos, result_cap - pos, "}");
    }
    pos += snprintf(result_buf + pos, result_cap - pos, "]");
    sqlite3_finalize(stmt);
    return pos;
}

/* ------------------------------------------------------------------ */
/*  QuickJS strata runner                                              */
/* ------------------------------------------------------------------ */

#include "quickjs.h"

/* JS binding: bedrock.log(msg) */
static JSValue js_bedrock_log(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *msg = JS_ToCString(ctx, argv[0]);
    if (msg) {
        fprintf(stderr, "[js] %s\n", msg);
        JS_FreeCString(ctx, msg);
    }
    return JS_UNDEFINED;
}

/* JS binding: bedrock.publish(topic, payload) -> subscribers reached or -1 */
static JSValue js_bedrock_publish(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->pub_hub || argc < 2) return JS_NewInt32(ctx, -1);

    const char *topic = JS_ToCString(ctx, argv[0]);
    const char *payload = JS_ToCString(ctx, argv[1]);
    int rc = -1;
    if (topic && payload) {
        rc = strata_pub_send(bedrock->pub_hub,
                             topic, strlen(topic),
                             payload, strlen(payload));
    }
    if (topic) JS_FreeCString(ctx, topic);
    if (payload) JS_FreeCString(ctx, payload);
    return JS_NewInt32(ctx, rc);
}

/* JS binding: bedrock.request(json [, endpoint]) -> response string or null
 * 1 arg:  sends to store (default req_endpoint)
 * 2 args: sends to arbitrary den/vocation endpoint */
static JSValue js_bedrock_request(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || argc < 1) return JS_NULL;

    const char *req = JS_ToCString(ctx, argv[0]);
    if (!req) return JS_NULL;

    const char *endpoint = NULL;
    if (argc >= 2) {
        endpoint = JS_ToCString(ctx, argv[1]);
        if (!endpoint) { JS_FreeCString(ctx, req); return JS_NULL; }
    } else {
        if (!bedrock->req_endpoint[0]) {
            JS_FreeCString(ctx, req);
            return JS_NULL;
        }
    }

    const char *target = endpoint ? endpoint : bedrock->req_endpoint;
    strata_sock *sock = strata_req_connect(target);
    if (!sock) {
        JS_FreeCString(ctx, req);
        if (endpoint) JS_FreeCString(ctx, endpoint);
        return JS_NULL;
    }

    /* Peer requests get longer timeout */
    if (endpoint)
        strata_msg_set_timeout(sock, 60000, 60000);
    else
        strata_msg_set_timeout(sock, 5000, 5000);

    int rc = strata_send(sock, req, strlen(req), 0);
    JS_FreeCString(ctx, req);
    if (endpoint) JS_FreeCString(ctx, endpoint);
    if (rc < 0) { strata_sock_close(sock); return JS_NULL; }

    /* Dynamic receive buffer — peer responses (e.g. API calls) can be large */
    size_t buf_cap = (argc >= 2) ? (4 * 1024 * 1024) : 8192;
    char *resp = malloc(buf_cap);
    if (!resp) { strata_sock_close(sock); return JS_NULL; }

    rc = strata_recv(sock, resp, buf_cap - 1, 0);
    strata_sock_close(sock);
    if (rc < 0) { free(resp); return JS_NULL; }
    resp[rc] = '\0';
    JSValue result = JS_NewString(ctx, resp);
    free(resp);
    return result;
}

/* JS binding: bedrock.serve_recv() -> request string or null */
static JSValue js_bedrock_serve_recv(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->rep_listener) return JS_NULL;

    /* Accept a new client connection */
    strata_sock *client = strata_rep_accept(bedrock->rep_listener);
    if (!client) return JS_NULL;

    char buf[8192];
    int rc = strata_recv(client, buf, sizeof(buf) - 1, 0);
    if (rc < 0) {
        strata_sock_close(client);
        return JS_NULL;
    }
    buf[rc] = '\0';
    /* Store client for the subsequent serve_send */
    bedrock->rep_client = client;
    return JS_NewString(ctx, buf);
}

/* JS binding: bedrock.serve_send(response) -> bytes sent or -1 */
static JSValue js_bedrock_serve_send(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->rep_client || argc < 1) return JS_NewInt32(ctx, -1);

    const char *resp = JS_ToCString(ctx, argv[0]);
    if (!resp) return JS_NewInt32(ctx, -1);

    int rc = strata_send(bedrock->rep_client, resp, strlen(resp), 0);
    JS_FreeCString(ctx, resp);
    strata_sock_close(bedrock->rep_client);
    bedrock->rep_client = NULL;
    return JS_NewInt32(ctx, rc);
}

/* JS binding: bedrock.subscribe(filter) */
static JSValue js_bedrock_subscribe(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    (void)this_val;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->sub_sock || argc < 1) return JS_NewInt32(ctx, -1);

    const char *filter = JS_ToCString(ctx, argv[0]);
    if (!filter) return JS_NewInt32(ctx, -1);

    int rc = strata_sub_subscribe(bedrock->sub_sock, filter);
    JS_FreeCString(ctx, filter);
    return JS_NewInt32(ctx, rc);
}

/* JS binding: bedrock.receive() -> {topic, payload} or null */
static JSValue js_bedrock_receive(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->sub_sock) return JS_NULL;

    char topic[512] = {0};
    char payload[8192] = {0};
    int rc = strata_sub_recv(bedrock->sub_sock,
                             topic, sizeof(topic),
                             payload, sizeof(payload));
    if (rc < 0) return JS_NULL;

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "topic", JS_NewString(ctx, topic));
    JS_SetPropertyStr(ctx, obj, "payload", JS_NewString(ctx, payload));
    return obj;
}

/* JS binding: bedrock.db_exec(sql) -> rows changed or null */
static JSValue js_bedrock_db_exec(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->local_db || argc < 1) return JS_NULL;

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql) return JS_NULL;

    char *errmsg = NULL;
    int rc = sqlite3_exec(bedrock->local_db, sql, NULL, NULL, &errmsg);
    JS_FreeCString(ctx, sql);
    if (errmsg) {
        fprintf(stderr, "[js] db_exec: %s\n", errmsg);
        sqlite3_free(errmsg);
    }
    if (rc != SQLITE_OK) return JS_NULL;
    return JS_NewInt32(ctx, sqlite3_changes(bedrock->local_db));
}

/* JS binding: bedrock.db_query(sql) -> JSON string or null */
static JSValue js_bedrock_db_query(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->local_db || argc < 1) return JS_NULL;

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql) return JS_NULL;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(bedrock->local_db, sql, -1, &stmt, NULL);
    JS_FreeCString(ctx, sql);
    if (rc != SQLITE_OK) return JS_NULL;

    /* Build JSON array of row objects */
    size_t cap = 65536;
    char *buf = malloc(cap);
    if (!buf) { sqlite3_finalize(stmt); return JS_NULL; }

    int pos = 0;
    pos += snprintf(buf + pos, cap - pos, "[");
    int first_row = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW && pos < (int)cap - 2) {
        if (!first_row) pos += snprintf(buf + pos, cap - pos, ",");
        first_row = 0;
        pos += snprintf(buf + pos, cap - pos, "{");
        int ncols = sqlite3_column_count(stmt);
        for (int i = 0; i < ncols && pos < (int)cap - 2; i++) {
            if (i > 0) pos += snprintf(buf + pos, cap - pos, ",");
            const char *cn = sqlite3_column_name(stmt, i);
            int type = sqlite3_column_type(stmt, i);
            if (type == SQLITE_INTEGER)
                pos += snprintf(buf + pos, cap - pos,
                    "\"%s\":%lld", cn, sqlite3_column_int64(stmt, i));
            else if (type == SQLITE_FLOAT)
                pos += snprintf(buf + pos, cap - pos,
                    "\"%s\":%g", cn, sqlite3_column_double(stmt, i));
            else if (type == SQLITE_NULL)
                pos += snprintf(buf + pos, cap - pos, "\"%s\":null", cn);
            else
                pos += snprintf(buf + pos, cap - pos,
                    "\"%s\":\"%s\"", cn, (const char *)sqlite3_column_text(stmt, i));
        }
        pos += snprintf(buf + pos, cap - pos, "}");
    }
    pos += snprintf(buf + pos, cap - pos, "]");
    sqlite3_finalize(stmt);

    JSValue result = JS_NewString(ctx, buf);
    free(buf);
    return result;
}

static void js_child_run(strata_den_def *def,
                         const char *event_json, int event_len) {
    bedrock_ctx_t bedrock;
    bedrock_setup(&bedrock, def);
    local_db_load(&bedrock, def);

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JS_SetContextOpaque(ctx, &bedrock);

    /* Create bedrock global object */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue bedrock_obj = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, bedrock_obj, "log",
        JS_NewCFunction(ctx, js_bedrock_log, "log", 1));
    JS_SetPropertyStr(ctx, bedrock_obj, "publish",
        JS_NewCFunction(ctx, js_bedrock_publish, "publish", 2));
    JS_SetPropertyStr(ctx, bedrock_obj, "request",
        JS_NewCFunction(ctx, js_bedrock_request, "request", 2));
    JS_SetPropertyStr(ctx, bedrock_obj, "serve_recv",
        JS_NewCFunction(ctx, js_bedrock_serve_recv, "serve_recv", 0));
    JS_SetPropertyStr(ctx, bedrock_obj, "serve_send",
        JS_NewCFunction(ctx, js_bedrock_serve_send, "serve_send", 1));
    JS_SetPropertyStr(ctx, bedrock_obj, "subscribe",
        JS_NewCFunction(ctx, js_bedrock_subscribe, "subscribe", 1));
    JS_SetPropertyStr(ctx, bedrock_obj, "receive",
        JS_NewCFunction(ctx, js_bedrock_receive, "receive", 0));
    JS_SetPropertyStr(ctx, bedrock_obj, "db_exec",
        JS_NewCFunction(ctx, js_bedrock_db_exec, "db_exec", 1));
    JS_SetPropertyStr(ctx, bedrock_obj, "db_query",
        JS_NewCFunction(ctx, js_bedrock_db_query, "db_query", 1));

    JS_SetPropertyStr(ctx, global, "bedrock", bedrock_obj);

    /* Set __event__ global with the trigger event */
    if (event_json && event_len > 0) {
        JSValue ev = JS_NewStringLen(ctx, event_json, event_len);
        JS_SetPropertyStr(ctx, global, "__event__", ev);
    }

    JS_FreeValue(ctx, global);

    /* Evaluate the JS source */
    JSValue result = JS_Eval(ctx, def->js_source, strlen(def->js_source),
                             def->js_path, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char *str = JS_ToCString(ctx, exc);
        fprintf(stderr, "[js] exception: %s\n", str ? str : "unknown");
        if (str) JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, result);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    local_db_save(&bedrock);
    bedrock_teardown(&bedrock);
}

/* ------------------------------------------------------------------ */
/*  Den host                                                         */
/* ------------------------------------------------------------------ */

struct strata_den_host {
    strata_den_def dens[STRATA_MAX_DENS];
    int den_count;
    strata_store *store;   /* optional — for privilege checks */
};

strata_den_host *strata_den_host_create(void) {
    return calloc(1, sizeof(strata_den_host));
}

void strata_den_host_free(strata_den_host *host) {
    if (!host) return;
    for (int i = 0; i < host->den_count; i++) {
        free(host->dens[i].c_source);
        free(host->dens[i].js_source);
    }
    free(host);
}

void strata_den_host_set_store(strata_den_host *host, strata_store *store) {
    if (host) host->store = store;
}

strata_store *strata_den_host_get_store(const strata_den_host *host) {
    return host ? host->store : NULL;
}

static char *load_text_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    if ((long)fread(buf, 1, len, f) != len) {
        free(buf); fclose(f); return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

int strata_den_register(strata_den_host *host,
                          const char *name, const char *c_path,
                          const char *trigger_filter,
                          const char *sub_endpoint, const char *req_endpoint) {
    if (!host || host->den_count >= STRATA_MAX_DENS) return -1;

    strata_den_def *def = &host->dens[host->den_count];
    memset(def, 0, sizeof(*def));
    def->mode = STRATA_MODE_NATIVE;
    strncpy(def->name, name, sizeof(def->name) - 1);
    strncpy(def->c_path, c_path, sizeof(def->c_path) - 1);
    if (trigger_filter) strncpy(def->trigger_filter, trigger_filter, sizeof(def->trigger_filter) - 1);
    if (sub_endpoint) strncpy(def->sub_endpoint, sub_endpoint, sizeof(def->sub_endpoint) - 1);
    if (req_endpoint) strncpy(def->req_endpoint, req_endpoint, sizeof(def->req_endpoint) - 1);

    def->c_source = load_text_file(c_path);
    if (!def->c_source) {
        fprintf(stderr, "failed to load %s\n", c_path);
        return -1;
    }
    def->c_source_len = strlen(def->c_source);
    host->den_count++;
    return 0;
}

int strata_den_js_register(strata_den_host *host,
                       const char *name, const char *js_path,
                       const char *sub_endpoint, const char *req_endpoint,
                       const char *pub_endpoint, const char *rep_endpoint) {
    if (!host || host->den_count >= STRATA_MAX_DENS) return -1;

    strata_den_def *def = &host->dens[host->den_count];
    memset(def, 0, sizeof(*def));
    def->mode = STRATA_MODE_JS;
    strncpy(def->name, name, sizeof(def->name) - 1);
    strncpy(def->js_path, js_path, sizeof(def->js_path) - 1);
    if (sub_endpoint) strncpy(def->sub_endpoint, sub_endpoint, sizeof(def->sub_endpoint) - 1);
    if (req_endpoint) strncpy(def->req_endpoint, req_endpoint, sizeof(def->req_endpoint) - 1);
    if (pub_endpoint) strncpy(def->pub_endpoint, pub_endpoint, sizeof(def->pub_endpoint) - 1);
    if (rep_endpoint) strncpy(def->rep_endpoint, rep_endpoint, sizeof(def->rep_endpoint) - 1);

    def->js_source = load_text_file(js_path);
    if (!def->js_source) {
        fprintf(stderr, "failed to load %s\n", js_path);
        return -1;
    }
    host->den_count++;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Native C child runner (TCC in-memory compilation)                  */
/* ------------------------------------------------------------------ */

static void native_child_run(strata_den_def *def,
                             const char *event_json, int event_len) {
    bedrock_ctx_t bedrock;
    bedrock_setup(&bedrock, def);
    local_db_load(&bedrock, def);
    g_bedrock_ctx = &bedrock;

    /* Apply OS sandbox before compiling/running untrusted code */
    strata_sandbox_apply();

    TCCState *s = tcc_new();
    if (!s) {
        fprintf(stderr, "[native] tcc_new failed\n");
        goto cleanup;
    }

    tcc_set_lib_path(s, "vendor/tcc");
    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);

    /* Suppress TCC warnings/errors to stderr via callback */
    tcc_set_error_func(s, NULL, NULL);

    /* Inject all bedrock symbols so den code can call them directly */
    tcc_add_symbol(s, "bedrock_log", native_bedrock_log);
    tcc_add_symbol(s, "bedrock_subscribe", native_bedrock_subscribe);
    tcc_add_symbol(s, "bedrock_receive", native_bedrock_receive);
    tcc_add_symbol(s, "bedrock_request", native_bedrock_request);
    tcc_add_symbol(s, "bedrock_request_to", native_bedrock_request_to);
    tcc_add_symbol(s, "bedrock_publish", native_bedrock_publish);
    tcc_add_symbol(s, "bedrock_serve_recv", native_bedrock_serve_recv);
    tcc_add_symbol(s, "bedrock_serve_send", native_bedrock_serve_send);
    tcc_add_symbol(s, "bedrock_db_exec", native_bedrock_db_exec);
    tcc_add_symbol(s, "bedrock_db_query", native_bedrock_db_query);

    /* Compile the C source */
    if (tcc_compile_string(s, def->c_source) == -1) {
        fprintf(stderr, "[native] compilation failed for den '%s'\n", def->name);
        goto cleanup_tcc;
    }

    if (tcc_relocate(s) < 0) {
        fprintf(stderr, "[native] relocate failed for den '%s'\n", def->name);
        goto cleanup_tcc;
    }

    /* Try serve() first (long-lived den), fall back to on_event */
    void (*serve_fn)(void) = tcc_get_symbol(s, "serve");
    if (serve_fn) {
        serve_fn();
    } else {
        void (*on_event_fn)(const char *, int) = tcc_get_symbol(s, "on_event");
        if (on_event_fn) {
            on_event_fn(event_json, event_len);
        } else {
            fprintf(stderr, "[native] no serve() or on_event() in den '%s'\n",
                    def->name);
        }
    }

cleanup_tcc:
    tcc_delete(s);
cleanup:
    local_db_save(&bedrock);
    bedrock_teardown(&bedrock);
}

/* ------------------------------------------------------------------ */
/*  Spawn: fork + dispatch by mode                                     */
/* ------------------------------------------------------------------ */

static strata_den_def *find_den(strata_den_host *host, const char *name) {
    for (int i = 0; i < host->den_count; i++) {
        if (strcmp(host->dens[i].name, name) == 0)
            return &host->dens[i];
    }
    return NULL;
}

pid_t strata_den_spawn(strata_den_host *host,
                         const char *den_name,
                         const char *event_json, int event_len) {
    if (!host || !den_name) return -1;

    strata_den_def *def = find_den(host, den_name);
    if (!def) {
        fprintf(stderr, "den '%s' not registered\n", den_name);
        return -1;
    }

    /* Privilege check: caller needs "parent" to spawn */
    if (host->store && def->den_id[0]) {
        if (!strata_has_privilege(host->store, def->den_id, "parent")) {
            fprintf(stderr, "den '%s' (id=%s): no 'parent' privilege\n",
                    den_name, def->den_id);
            return -1;
        }
    }

    /* Local copy — strip pub/sub if no vocation privilege */
    strata_den_def local_def = *def;
    if (host->store && def->den_id[0]) {
        if (!strata_has_privilege(host->store, def->den_id, "vocation")) {
            local_def.pub_endpoint[0] = '\0';
            local_def.sub_endpoint[0] = '\0';
        }
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }

    if (pid == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (local_def.mode == STRATA_MODE_JS)
            js_child_run(&local_def, event_json, event_len);
        else
            native_child_run(&local_def, event_json, event_len);
        _exit(0);
    }

    return pid;
}

int strata_den_host_reap(strata_den_host *host) {
    (void)host;
    int count = 0;
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0)
        count++;
    return count;
}

const strata_den_def *strata_den_host_find(const strata_den_host *host,
                                                const char *name) {
    if (!host || !name) return NULL;
    for (int i = 0; i < host->den_count; i++) {
        if (strcmp(host->dens[i].name, name) == 0)
            return &host->dens[i];
    }
    return NULL;
}

int strata_den_register_native_buf(strata_den_host *host,
                                     const char *name,
                                     const char *c_source, size_t c_len,
                                     const char *sub_endpoint,
                                     const char *req_endpoint) {
    if (!host || !name || !c_source || host->den_count >= STRATA_MAX_DENS) return -1;

    strata_den_def *def = &host->dens[host->den_count];
    memset(def, 0, sizeof(*def));
    def->mode = STRATA_MODE_NATIVE;
    strncpy(def->name, name, sizeof(def->name) - 1);
    if (sub_endpoint) strncpy(def->sub_endpoint, sub_endpoint, sizeof(def->sub_endpoint) - 1);
    if (req_endpoint) strncpy(def->req_endpoint, req_endpoint, sizeof(def->req_endpoint) - 1);

    def->c_source = strndup(c_source, c_len);
    if (!def->c_source) return -1;
    def->c_source_len = c_len;

    host->den_count++;
    return 0;
}

int strata_den_register_js_buf(strata_den_host *host,
                                  const char *name,
                                  const char *js_source,
                                  const char *sub_endpoint,
                                  const char *req_endpoint,
                                  const char *pub_endpoint,
                                  const char *rep_endpoint) {
    if (!host || !name || !js_source || host->den_count >= STRATA_MAX_DENS) return -1;

    strata_den_def *def = &host->dens[host->den_count];
    memset(def, 0, sizeof(*def));
    def->mode = STRATA_MODE_JS;
    strncpy(def->name, name, sizeof(def->name) - 1);
    if (sub_endpoint) strncpy(def->sub_endpoint, sub_endpoint, sizeof(def->sub_endpoint) - 1);
    if (req_endpoint) strncpy(def->req_endpoint, req_endpoint, sizeof(def->req_endpoint) - 1);
    if (pub_endpoint) strncpy(def->pub_endpoint, pub_endpoint, sizeof(def->pub_endpoint) - 1);
    if (rep_endpoint) strncpy(def->rep_endpoint, rep_endpoint, sizeof(def->rep_endpoint) - 1);

    def->js_source = strdup(js_source);
    if (!def->js_source) return -1;

    host->den_count++;
    return 0;
}
