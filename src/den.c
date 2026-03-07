#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <zmq.h>
#include <sqlite3.h>
#include <wasm_export.h>

#include "strata/den.h"
#include "strata/store.h"

/* ------------------------------------------------------------------ */
/*  Bedrock context — shared by WASM natives and QuickJS bindings   */
/* ------------------------------------------------------------------ */

typedef struct {
    void *zmq_ctx;
    void *sub_sock;
    void *req_sock;
    void *pub_sock;
    void *rep_sock;
    sqlite3 *local_db;
    char den_name[64];
    char den_entity[256];
    char local_db_path[256];
} bedrock_ctx_t;

/* ------------------------------------------------------------------ */
/*  ZMQ socket setup helpers                                           */
/* ------------------------------------------------------------------ */

static void bedrock_setup(bedrock_ctx_t *bedrock, strata_den_def *def) {
    memset(bedrock, 0, sizeof(*bedrock));
    bedrock->zmq_ctx = zmq_ctx_new();
    int timeout = 5000;

    if (def->sub_endpoint[0]) {
        bedrock->sub_sock = zmq_socket(bedrock->zmq_ctx, ZMQ_SUB);
        zmq_connect(bedrock->sub_sock, def->sub_endpoint);
        zmq_setsockopt(bedrock->sub_sock, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    }
    if (def->req_endpoint[0]) {
        bedrock->req_sock = zmq_socket(bedrock->zmq_ctx, ZMQ_REQ);
        zmq_connect(bedrock->req_sock, def->req_endpoint);
        zmq_setsockopt(bedrock->req_sock, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    }
    if (def->pub_endpoint[0]) {
        bedrock->pub_sock = zmq_socket(bedrock->zmq_ctx, ZMQ_PUB);
        zmq_bind(bedrock->pub_sock, def->pub_endpoint);
    }
    if (def->rep_endpoint[0]) {
        bedrock->rep_sock = zmq_socket(bedrock->zmq_ctx, ZMQ_REP);
        zmq_bind(bedrock->rep_sock, def->rep_endpoint);
        zmq_setsockopt(bedrock->rep_sock, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    }
}

static void bedrock_teardown(bedrock_ctx_t *bedrock) {
    if (bedrock->local_db) sqlite3_close(bedrock->local_db);
    if (bedrock->sub_sock) zmq_close(bedrock->sub_sock);
    if (bedrock->req_sock) zmq_close(bedrock->req_sock);
    if (bedrock->pub_sock) zmq_close(bedrock->pub_sock);
    if (bedrock->rep_sock) zmq_close(bedrock->rep_sock);
    if (bedrock->zmq_ctx)  zmq_ctx_destroy(bedrock->zmq_ctx);
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
    if (bedrock->req_sock) {
        char req[512];
        snprintf(req, sizeof(req),
            "{\"action\":\"blob_find\",\"entity\":\"%s\",\"tags\":[\"den:%s:db\"]}",
            bedrock->den_entity, bedrock->den_name);

        int rc = zmq_send(bedrock->req_sock, req, strlen(req), 0);
        if (rc >= 0) {
            /* Use large buffer for db content */
            size_t resp_cap = 4 * 1024 * 1024;  /* 4 MB */
            char *resp = malloc(resp_cap);
            if (resp) {
                int old_timeout = 0;
                size_t opt_len = sizeof(old_timeout);
                zmq_getsockopt(bedrock->req_sock, ZMQ_RCVTIMEO, &old_timeout, &opt_len);
                int load_timeout = 30000;
                zmq_setsockopt(bedrock->req_sock, ZMQ_RCVTIMEO, &load_timeout, sizeof(load_timeout));

                rc = zmq_recv(bedrock->req_sock, resp, resp_cap - 1, 0);
                zmq_setsockopt(bedrock->req_sock, ZMQ_RCVTIMEO, &old_timeout, sizeof(old_timeout));

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

    if (!bedrock->req_sock) goto cleanup_file;

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

        int rc = zmq_send(bedrock->req_sock, req, strlen(req), 0);
        if (rc >= 0) {
            char resp[1024];
            zmq_recv(bedrock->req_sock, resp, sizeof(resp) - 1, 0);
            fprintf(stderr, "[%s] saved db (%ld bytes)\n", bedrock->den_name, flen);
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
/*  WASM native functions                                              */
/* ------------------------------------------------------------------ */

static void wasm_bedrock_log(wasm_exec_env_t exec_env,
                          char *msg, int32_t msg_len) {
    (void)exec_env;
    fprintf(stderr, "[wasm] %.*s\n", msg_len, msg);
}

static int32_t wasm_bedrock_subscribe(wasm_exec_env_t exec_env,
                                   char *filter, int32_t filter_len) {
    bedrock_ctx_t *bedrock = wasm_runtime_get_function_attachment(exec_env);
    if (!bedrock || !bedrock->sub_sock) return -1;
    return zmq_setsockopt(bedrock->sub_sock, ZMQ_SUBSCRIBE, filter, filter_len);
}

static int32_t wasm_bedrock_receive(wasm_exec_env_t exec_env,
                                 char *topic_buf, int32_t topic_cap,
                                 char *payload_buf, int32_t payload_cap) {
    bedrock_ctx_t *bedrock = wasm_runtime_get_function_attachment(exec_env);
    if (!bedrock || !bedrock->sub_sock) return -1;
    int rc = zmq_recv(bedrock->sub_sock, topic_buf, topic_cap, 0);
    if (rc < 0) return -1;
    return zmq_recv(bedrock->sub_sock, payload_buf, payload_cap, 0);
}

static int32_t wasm_bedrock_request(wasm_exec_env_t exec_env,
                                 char *req, int32_t req_len,
                                 char *resp_buf, int32_t resp_cap) {
    bedrock_ctx_t *bedrock = wasm_runtime_get_function_attachment(exec_env);
    if (!bedrock || !bedrock->req_sock) return -1;
    int rc = zmq_send(bedrock->req_sock, req, req_len, 0);
    if (rc < 0) return -1;
    return zmq_recv(bedrock->req_sock, resp_buf, resp_cap, 0);
}

static int32_t wasm_bedrock_publish(wasm_exec_env_t exec_env,
                                 char *topic, int32_t topic_len,
                                 char *payload, int32_t payload_len) {
    bedrock_ctx_t *bedrock = wasm_runtime_get_function_attachment(exec_env);
    if (!bedrock || !bedrock->pub_sock) return -1;
    zmq_send(bedrock->pub_sock, topic, topic_len, ZMQ_SNDMORE);
    return zmq_send(bedrock->pub_sock, payload, payload_len, 0);
}

static int32_t wasm_bedrock_serve_recv(wasm_exec_env_t exec_env,
                                    char *buf, int32_t cap) {
    bedrock_ctx_t *bedrock = wasm_runtime_get_function_attachment(exec_env);
    if (!bedrock || !bedrock->rep_sock) return -1;
    return zmq_recv(bedrock->rep_sock, buf, cap, 0);
}

static int32_t wasm_bedrock_serve_send(wasm_exec_env_t exec_env,
                                    char *resp, int32_t resp_len) {
    bedrock_ctx_t *bedrock = wasm_runtime_get_function_attachment(exec_env);
    if (!bedrock || !bedrock->rep_sock) return -1;
    return zmq_send(bedrock->rep_sock, resp, resp_len, 0);
}

static int32_t wasm_bedrock_db_exec(wasm_exec_env_t exec_env,
                                    char *sql, int32_t sql_len) {
    bedrock_ctx_t *bedrock = wasm_runtime_get_function_attachment(exec_env);
    if (!bedrock || !bedrock->local_db) return -1;
    char *sql_z = strndup(sql, sql_len);
    if (!sql_z) return -1;
    char *errmsg = NULL;
    int rc = sqlite3_exec(bedrock->local_db, sql_z, NULL, NULL, &errmsg);
    free(sql_z);
    if (errmsg) { fprintf(stderr, "[wasm] db_exec: %s\n", errmsg); sqlite3_free(errmsg); }
    if (rc != SQLITE_OK) return -1;
    return sqlite3_changes(bedrock->local_db);
}

static int32_t wasm_bedrock_db_query(wasm_exec_env_t exec_env,
                                     char *sql, int32_t sql_len,
                                     char *result_buf, int32_t result_cap) {
    bedrock_ctx_t *bedrock = wasm_runtime_get_function_attachment(exec_env);
    if (!bedrock || !bedrock->local_db) return -1;
    char *sql_z = strndup(sql, sql_len);
    if (!sql_z) return -1;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(bedrock->local_db, sql_z, -1, &stmt, NULL);
    free(sql_z);
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

/* JS binding: bedrock.publish(topic, payload) -> bytes sent or -1 */
static JSValue js_bedrock_publish(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->pub_sock || argc < 2) return JS_NewInt32(ctx, -1);

    const char *topic = JS_ToCString(ctx, argv[0]);
    const char *payload = JS_ToCString(ctx, argv[1]);
    int rc = -1;
    if (topic && payload) {
        zmq_send(bedrock->pub_sock, topic, strlen(topic), ZMQ_SNDMORE);
        rc = zmq_send(bedrock->pub_sock, payload, strlen(payload), 0);
    }
    if (topic) JS_FreeCString(ctx, topic);
    if (payload) JS_FreeCString(ctx, payload);
    return JS_NewInt32(ctx, rc);
}

/* JS binding: bedrock.request(msg) -> response string or null */
static JSValue js_bedrock_request(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->req_sock || argc < 1) return JS_NULL;

    const char *req = JS_ToCString(ctx, argv[0]);
    if (!req) return JS_NULL;

    int rc = zmq_send(bedrock->req_sock, req, strlen(req), 0);
    JS_FreeCString(ctx, req);
    if (rc < 0) return JS_NULL;

    char resp[8192];
    rc = zmq_recv(bedrock->req_sock, resp, sizeof(resp) - 1, 0);
    if (rc < 0) return JS_NULL;
    resp[rc] = '\0';
    return JS_NewString(ctx, resp);
}

/* JS binding: bedrock.serve_recv() -> request string or null */
static JSValue js_bedrock_serve_recv(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->rep_sock) return JS_NULL;

    char buf[8192];
    int rc = zmq_recv(bedrock->rep_sock, buf, sizeof(buf) - 1, 0);
    if (rc < 0) return JS_NULL;
    buf[rc] = '\0';
    return JS_NewString(ctx, buf);
}

/* JS binding: bedrock.serve_send(response) -> bytes sent or -1 */
static JSValue js_bedrock_serve_send(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    bedrock_ctx_t *bedrock = JS_GetContextOpaque(ctx);
    if (!bedrock || !bedrock->rep_sock || argc < 1) return JS_NewInt32(ctx, -1);

    const char *resp = JS_ToCString(ctx, argv[0]);
    if (!resp) return JS_NewInt32(ctx, -1);

    int rc = zmq_send(bedrock->rep_sock, resp, strlen(resp), 0);
    JS_FreeCString(ctx, resp);
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

    int rc = zmq_setsockopt(bedrock->sub_sock, ZMQ_SUBSCRIBE, filter, strlen(filter));
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
    int rc = zmq_recv(bedrock->sub_sock, topic, sizeof(topic) - 1, 0);
    if (rc < 0) return JS_NULL;
    topic[rc < (int)sizeof(topic) ? rc : (int)sizeof(topic) - 1] = '\0';

    rc = zmq_recv(bedrock->sub_sock, payload, sizeof(payload) - 1, 0);
    if (rc < 0) return JS_NULL;
    payload[rc < (int)sizeof(payload) ? rc : (int)sizeof(payload) - 1] = '\0';

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
        JS_NewCFunction(ctx, js_bedrock_request, "request", 1));
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
        free(host->dens[i].wasm_buf);
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

static unsigned char *load_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(len);
    if (!buf) { fclose(f); return NULL; }
    if ((long)fread(buf, 1, len, f) != len) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_len = (size_t)len;
    return buf;
}

int strata_den_register(strata_den_host *host,
                          const char *name, const char *wasm_path,
                          const char *trigger_filter,
                          const char *sub_endpoint, const char *req_endpoint) {
    if (!host || host->den_count >= STRATA_MAX_DENS) return -1;

    strata_den_def *def = &host->dens[host->den_count];
    memset(def, 0, sizeof(*def));
    def->mode = STRATA_MODE_WASM;
    strncpy(def->name, name, sizeof(def->name) - 1);
    strncpy(def->wasm_path, wasm_path, sizeof(def->wasm_path) - 1);
    if (trigger_filter) strncpy(def->trigger_filter, trigger_filter, sizeof(def->trigger_filter) - 1);
    if (sub_endpoint) strncpy(def->sub_endpoint, sub_endpoint, sizeof(def->sub_endpoint) - 1);
    if (req_endpoint) strncpy(def->req_endpoint, req_endpoint, sizeof(def->req_endpoint) - 1);

    def->wasm_buf = load_file(wasm_path, &def->wasm_len);
    if (!def->wasm_buf) {
        fprintf(stderr, "failed to load %s\n", wasm_path);
        return -1;
    }
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
/*  WASM child runner                  */
/* ------------------------------------------------------------------ */

static void wasm_child_run(strata_den_def *def,
                           const char *event_json, int event_len) {
    char err[128];
    bedrock_ctx_t bedrock;
    bedrock_setup(&bedrock, def);
    local_db_load(&bedrock, def);

    if (!wasm_runtime_init()) {
        fprintf(stderr, "wasm_runtime_init failed\n");
        goto cleanup;
    }

    static NativeSymbol natives[] = {
        { "log",        (void *)wasm_bedrock_log,        "(*~)",     NULL },
        { "subscribe",  (void *)wasm_bedrock_subscribe,   "(*~)i",    NULL },
        { "receive",    (void *)wasm_bedrock_receive,     "(*~*~)i",  NULL },
        { "request",    (void *)wasm_bedrock_request,     "(*~*~)i",  NULL },
        { "publish",    (void *)wasm_bedrock_publish,     "(*~*~)i",  NULL },
        { "serve_recv", (void *)wasm_bedrock_serve_recv,  "(*~)i",    NULL },
        { "serve_send", (void *)wasm_bedrock_serve_send,  "(*~)i",    NULL },
        { "db_exec",    (void *)wasm_bedrock_db_exec,     "(*~)i",    NULL },
        { "db_query",   (void *)wasm_bedrock_db_query,    "(*~*~)i",  NULL },
    };
    for (int i = 0; i < 9; i++) natives[i].attachment = &bedrock;

    if (!wasm_runtime_register_natives("bedrock", natives, 9)) {
        fprintf(stderr, "register_natives failed\n");
        goto cleanup_wamr;
    }

    unsigned char *wasm_copy = malloc(def->wasm_len);
    if (!wasm_copy) goto cleanup_wamr;
    memcpy(wasm_copy, def->wasm_buf, def->wasm_len);

    wasm_module_t module = wasm_runtime_load(wasm_copy, (uint32_t)def->wasm_len,
                                             err, sizeof(err));
    if (!module) {
        fprintf(stderr, "wasm_runtime_load: %s\n", err);
        free(wasm_copy);
        goto cleanup_wamr;
    }

    wasm_module_inst_t inst = wasm_runtime_instantiate(module, 16384, 16384,
                                                       err, sizeof(err));
    if (!inst) {
        fprintf(stderr, "wasm_runtime_instantiate: %s\n", err);
        goto cleanup_module;
    }

    wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(inst, 16384);
    if (!exec_env) goto cleanup_inst;

    /* Try serve() first (long-lived strata), fall back to on_event */
    wasm_function_inst_t func = wasm_runtime_lookup_function(inst, "serve");
    if (func) {
        wasm_runtime_call_wasm(exec_env, func, 0, NULL);
    } else {
        func = wasm_runtime_lookup_function(inst, "on_event");
        if (!func) {
            fprintf(stderr, "no serve() or on_event() in WASM module\n");
            goto cleanup_env;
        }
        void *native_ptr = NULL;
        uint32_t wasm_ptr = wasm_runtime_module_malloc(inst, event_len, &native_ptr);
        if (!wasm_ptr) goto cleanup_env;
        memcpy(native_ptr, event_json, event_len);

        uint32_t argv[2] = { wasm_ptr, (uint32_t)event_len };
        if (!wasm_runtime_call_wasm(exec_env, func, 2, argv)) {
            const char *exc = wasm_runtime_get_exception(inst);
            fprintf(stderr, "on_event failed: %s\n", exc ? exc : "unknown");
        }
        wasm_runtime_module_free(inst, wasm_ptr);
    }

cleanup_env:
    wasm_runtime_destroy_exec_env(exec_env);
cleanup_inst:
    wasm_runtime_deinstantiate(inst);
cleanup_module:
    wasm_runtime_unload(module);
    free(wasm_copy);
cleanup_wamr:
    wasm_runtime_destroy();
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
            wasm_child_run(&local_def, event_json, event_len);
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

int strata_den_register_wasm_buf(strata_den_host *host,
                                    const char *name,
                                    const unsigned char *wasm_buf, size_t wasm_len,
                                    const char *sub_endpoint,
                                    const char *req_endpoint) {
    if (!host || !name || !wasm_buf || host->den_count >= STRATA_MAX_DENS) return -1;

    strata_den_def *def = &host->dens[host->den_count];
    memset(def, 0, sizeof(*def));
    def->mode = STRATA_MODE_WASM;
    strncpy(def->name, name, sizeof(def->name) - 1);
    if (sub_endpoint) strncpy(def->sub_endpoint, sub_endpoint, sizeof(def->sub_endpoint) - 1);
    if (req_endpoint) strncpy(def->req_endpoint, req_endpoint, sizeof(def->req_endpoint) - 1);

    def->wasm_buf = malloc(wasm_len);
    if (!def->wasm_buf) return -1;
    memcpy(def->wasm_buf, wasm_buf, wasm_len);
    def->wasm_len = wasm_len;

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
