/*
 * cobbler — a vocation den that validates and compiles C source using TCC.
 *
 * JSON-over-ZMQ-REP service. Same pattern as code-smith.
 *
 * Actions: discover, compile, compile_file
 *
 * Usage:
 *   cobbler --endpoint tcp://127.0.0.1:5591 --root /path/to/project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <zmq.h>
#include <libtcc.h>

#include "strata/json_util.h"

#define MAX_SOURCE_SIZE  (1 * 1024 * 1024)   /* 1MB source cap */
#define RESP_CAP         (2 * 1024 * 1024)   /* 2MB response cap */

static volatile int running = 1;
static char root_path[PATH_MAX] = ".";

static void sigint_handler(int sig) {
    (void)sig;
    running = 0;
}

/* ------------------------------------------------------------------ */
/*  Path safety — resolve relative to root, reject escapes             */
/* ------------------------------------------------------------------ */

static int safe_path(const char *user_path, char *out, size_t out_cap) {
    if (!user_path || !user_path[0]) {
        snprintf(out, out_cap, "%s", root_path);
        return 0;
    }

    char combined[PATH_MAX];
    if (user_path[0] == '/')
        snprintf(combined, sizeof(combined), "%s%s", root_path, user_path);
    else
        snprintf(combined, sizeof(combined), "%s/%s", root_path, user_path);

    char resolved[PATH_MAX];
    if (!realpath(combined, resolved)) {
        char *slash = strrchr(combined, '/');
        if (slash) {
            char parent[PATH_MAX];
            size_t plen = (size_t)(slash - combined);
            if (plen >= sizeof(parent)) return -1;
            memcpy(parent, combined, plen);
            parent[plen] = '\0';
            if (!realpath(parent, resolved)) return -1;
            size_t rlen = strlen(resolved);
            snprintf(resolved + rlen, sizeof(resolved) - rlen, "%s", slash);
        } else {
            return -1;
        }
    }

    size_t rlen = strlen(root_path);
    if (strncmp(resolved, root_path, rlen) != 0)
        return -1;
    if (resolved[rlen] != '\0' && resolved[rlen] != '/')
        return -1;

    snprintf(out, out_cap, "%s", resolved);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  TCC error collection                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char buf[4096];
    int pos;
} tcc_err_ctx_t;

static void tcc_error_handler(void *opaque, const char *msg) {
    tcc_err_ctx_t *ctx = opaque;
    if (!ctx || !msg) return;
    int remaining = (int)sizeof(ctx->buf) - ctx->pos - 1;
    if (remaining <= 0) return;
    if (ctx->pos > 0) {
        ctx->buf[ctx->pos++] = '\n';
        remaining--;
    }
    int n = snprintf(ctx->buf + ctx->pos, remaining, "%s", msg);
    if (n > 0) ctx->pos += (n < remaining ? n : remaining);
}

/* ------------------------------------------------------------------ */
/*  Symbol scanning for entry point detection                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int has_serve;
    int has_on_event;
} sym_scan_t;

static void sym_scan_cb(void *ctx, const char *name, const void *val) {
    (void)val;
    sym_scan_t *scan = ctx;
    /* macOS prefixes symbols with underscore */
    if (name[0] == '_') name++;
    if (strcmp(name, "serve") == 0) scan->has_serve = 1;
    if (strcmp(name, "on_event") == 0) scan->has_on_event = 1;
}

/* ------------------------------------------------------------------ */
/*  Compile using TCC in-memory                                         */
/* ------------------------------------------------------------------ */

static void do_compile_source(const char *source, int source_len,
                              char *resp, int resp_cap) {
    tcc_err_ctx_t err_ctx;
    memset(&err_ctx, 0, sizeof(err_ctx));

    TCCState *s = tcc_new();
    if (!s) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"tcc_new failed\"}");
        return;
    }

    tcc_set_lib_path(s, "vendor/tcc");
    tcc_set_output_type(s, TCC_OUTPUT_OBJ);
    tcc_set_error_func(s, &err_ctx, tcc_error_handler);
    tcc_set_options(s, "-nostdlib");

    int rc = tcc_compile_string(s, source);
    if (rc == -1) {
        char esc_err[8192];
        json_escape(err_ctx.buf, err_ctx.pos, esc_err, sizeof(esc_err));
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"%s\"}", esc_err);
        tcc_delete(s);
        return;
    }

    /* Scan symbols for entry points */
    sym_scan_t scan = {0, 0};
    tcc_list_symbols(s, &scan, sym_scan_cb);
    tcc_delete(s);

    snprintf(resp, resp_cap,
        "{\"ok\":true,\"valid\":true,\"size\":%d,"
        "\"has_serve\":%s,\"has_on_event\":%s}",
        source_len,
        scan.has_serve ? "true" : "false",
        scan.has_on_event ? "true" : "false");
}

/* ------------------------------------------------------------------ */
/*  Action handlers                                                     */
/* ------------------------------------------------------------------ */

static void handle_discover(char *resp, int resp_cap) {
    snprintf(resp, resp_cap,
        "{\"ok\":true,\"name\":\"cobbler\",\"actions\":{"
        "\"discover\":{\"params\":{}},"
        "\"compile\":{\"params\":{\"source\":\"string\"}},"
        "\"compile_file\":{\"params\":{\"path\":\"string\"}}"
        "},\"compiler\":\"tcc\",\"root\":\"%s\"}",
        root_path);
}

static void handle_compile(const char *req, char *resp, int resp_cap) {
    char *source = malloc(MAX_SOURCE_SIZE + 1);
    if (!source) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"oom\"}");
        return;
    }
    int slen = json_get_string(req, "source", source, MAX_SOURCE_SIZE);
    if (slen <= 0) {
        free(source);
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"source required\"}");
        return;
    }

    do_compile_source(source, slen, resp, resp_cap);
    free(source);
}

static void handle_compile_file(const char *req, char *resp, int resp_cap) {
    char path[256] = {0};
    json_get_string(req, "path", path, sizeof(path));
    if (!path[0]) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"path required\"}");
        return;
    }

    char safe[PATH_MAX];
    if (safe_path(path, safe, sizeof(safe)) != 0) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"path not allowed\"}");
        return;
    }

    if (access(safe, R_OK) != 0) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"cannot read file\"}");
        return;
    }

    struct stat st;
    if (stat(safe, &st) != 0 || st.st_size > MAX_SOURCE_SIZE) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"file too large\"}");
        return;
    }

    FILE *f = fopen(safe, "r");
    if (!f) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"cannot open file\"}");
        return;
    }

    char *source = malloc(st.st_size + 1);
    if (!source) {
        fclose(f);
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"oom\"}");
        return;
    }

    size_t nr = fread(source, 1, st.st_size, f);
    fclose(f);
    source[nr] = '\0';

    do_compile_source(source, (int)nr, resp, resp_cap);
    free(source);
}

/* ------------------------------------------------------------------ */
/*  Request dispatch                                                    */
/* ------------------------------------------------------------------ */

static void handle_request(const char *req, int req_len,
                           char *resp, int resp_cap) {
    (void)req_len;
    char action[32] = {0};
    json_get_string(req, "action", action, sizeof(action));

    if (strcmp(action, "init") == 0) {
        snprintf(resp, resp_cap, "{\"ok\":true,\"name\":\"cobbler\"}");
    } else if (strcmp(action, "say") == 0) {
        char msg[8192] = {0};
        json_get_string(req, "message", msg, sizeof(msg));
        if (msg[0] == '{') {
            handle_request(msg, (int)strlen(msg), resp, resp_cap);
        } else if (msg[0]) {
            snprintf(resp, resp_cap,
                "{\"ok\":true,\"message\":\"cobbler validates C source using TCC. "
                "Send {\\\"action\\\":\\\"compile\\\",\\\"source\\\":\\\"...\\\"}  "
                "or {\\\"action\\\":\\\"discover\\\"} for details.\"}");
        } else {
            handle_discover(resp, resp_cap);
        }
    } else if (strcmp(action, "discover") == 0)
        handle_discover(resp, resp_cap);
    else if (strcmp(action, "compile") == 0)
        handle_compile(req, resp, resp_cap);
    else if (strcmp(action, "compile_file") == 0)
        handle_compile_file(req, resp, resp_cap);
    else
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"unknown action: %s\"}", action);
}

/* ------------------------------------------------------------------ */
/*  Service main loop                                                   */
/* ------------------------------------------------------------------ */

int cobbler_run(const char *endpoint, const char *root, const char *clang) {
    (void)clang; /* no longer used — kept for API compat */
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    if (!realpath(root, root_path)) {
        fprintf(stderr, "cobbler: cannot resolve root '%s'\n", root);
        return 1;
    }

    fprintf(stderr, "cobbler: using vendored TCC compiler\n");

    void *zmq_ctx = zmq_ctx_new();
    void *rep = zmq_socket(zmq_ctx, ZMQ_REP);
    zmq_bind(rep, endpoint);

    int timeout = 1000;
    zmq_setsockopt(rep, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));

    fprintf(stderr, "cobbler: listening on %s  root=%s\n", endpoint, root_path);

    char *req_buf = malloc(MAX_SOURCE_SIZE + 4096);
    char *resp_buf = malloc(RESP_CAP);
    if (!req_buf || !resp_buf) {
        fprintf(stderr, "cobbler: malloc failed\n");
        free(req_buf); free(resp_buf);
        zmq_close(rep); zmq_ctx_destroy(zmq_ctx);
        return 1;
    }

    while (running) {
        int rc = zmq_recv(rep, req_buf, MAX_SOURCE_SIZE + 4095, 0);
        if (rc < 0) continue;
        req_buf[rc] = '\0';

        resp_buf[0] = '\0';
        handle_request(req_buf, rc, resp_buf, RESP_CAP);
        zmq_send(rep, resp_buf, strlen(resp_buf), 0);
    }

    free(req_buf);
    free(resp_buf);
    zmq_close(rep);
    zmq_ctx_destroy(zmq_ctx);
    fprintf(stderr, "cobbler: shutdown\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Standalone entry point                                              */
/* ------------------------------------------------------------------ */

#ifndef COBBLER_NO_MAIN
int main(int argc, char **argv) {
    const char *endpoint = "tcp://127.0.0.1:5591";
    const char *root = ".";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--endpoint") == 0 && i + 1 < argc)
            endpoint = argv[++i];
        else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc)
            root = argv[++i];
        else {
            fprintf(stderr, "usage: cobbler [--endpoint EP] [--root PATH]\n");
            return 1;
        }
    }

    return cobbler_run(endpoint, root, NULL);
}
#endif
