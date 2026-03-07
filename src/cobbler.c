/*
 * cobbler — a vocation den that compiles C source to WASM binaries.
 *
 * JSON-over-ZMQ-REP service. Same pattern as code-smith.
 *
 * Actions: discover, compile, compile_file
 *
 * Usage:
 *   cobbler --endpoint tcp://127.0.0.1:5591 --root /path/to/project
 *   cobbler --endpoint tcp://127.0.0.1:5591 --root . --clang /opt/homebrew/opt/llvm/bin/clang
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <zmq.h>

#include "strata/json_util.h"

#define MAX_SOURCE_SIZE  (1 * 1024 * 1024)   /* 1MB source cap */
#define MAX_WASM_OUTPUT  (4 * 1024 * 1024)   /* 4MB wasm output cap */
#define RESP_CAP         (8 * 1024 * 1024)   /* 8MB response (base64 expands) */
#define MAX_EXPORTS      32

static volatile int running = 1;
static char root_path[PATH_MAX] = ".";
static char clang_path[PATH_MAX] = "";

static void sigint_handler(int sig) {
    (void)sig;
    running = 0;
}

/* ------------------------------------------------------------------ */
/*  Base64 encoding                                                     */
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
        /* File might not exist yet (for write). Resolve parent. */
        char *slash = strrchr(combined, '/');
        if (slash) {
            char parent[PATH_MAX];
            size_t plen = (size_t)(slash - combined);
            if (plen >= sizeof(parent)) return -1;
            memcpy(parent, combined, plen);
            parent[plen] = '\0';
            if (!realpath(parent, resolved)) return -1;
            /* Append filename */
            size_t rlen = strlen(resolved);
            snprintf(resolved + rlen, sizeof(resolved) - rlen, "%s", slash);
        } else {
            return -1;
        }
    }

    /* Must start with root_path */
    size_t rlen = strlen(root_path);
    if (strncmp(resolved, root_path, rlen) != 0)
        return -1;
    /* Next char must be '/' or '\0' */
    if (resolved[rlen] != '\0' && resolved[rlen] != '/')
        return -1;

    snprintf(out, out_cap, "%s", resolved);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Clang detection                                                     */
/* ------------------------------------------------------------------ */

static int find_clang(void) {
    /* 1. Already set via --clang */
    if (clang_path[0]) {
        if (access(clang_path, X_OK) == 0)
            return 0;
        fprintf(stderr, "cobbler: specified clang not found: %s\n", clang_path);
        return -1;
    }

    /* 2. Homebrew LLVM */
    const char *brew = "/opt/homebrew/opt/llvm/bin/clang";
    if (access(brew, X_OK) == 0) {
        snprintf(clang_path, sizeof(clang_path), "%s", brew);
        return 0;
    }

    /* 3. System PATH — find full path */
    FILE *p = popen("which clang 2>/dev/null", "r");
    if (p) {
        if (fgets(clang_path, sizeof(clang_path), p)) {
            /* Strip trailing newline */
            size_t len = strlen(clang_path);
            if (len > 0 && clang_path[len - 1] == '\n')
                clang_path[len - 1] = '\0';
        }
        pclose(p);
        if (clang_path[0] && access(clang_path, X_OK) == 0)
            return 0;
    }

    fprintf(stderr, "cobbler: clang not found\n");
    return -1;
}

static int verify_wasm_target(void) {
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd),
        "%s --target=wasm32 -x c -c -o /dev/null /dev/null 2>/dev/null",
        clang_path);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "cobbler: clang at %s does not support --target=wasm32\n", clang_path);
        return -1;
    }
    return 0;
}

static void get_clang_version(char *out, size_t out_cap) {
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "%s --version 2>/dev/null | head -1", clang_path);
    FILE *p = popen(cmd, "r");
    if (p) {
        if (fgets(out, (int)out_cap, p)) {
            size_t len = strlen(out);
            if (len > 0 && out[len - 1] == '\n')
                out[len - 1] = '\0';
        }
        pclose(p);
    }
    if (!out[0])
        snprintf(out, out_cap, "unknown");
}

/* ------------------------------------------------------------------ */
/*  Compile helpers                                                     */
/* ------------------------------------------------------------------ */

static void do_compile(const char *src_path, const char *exports[], int nexports,
                       char *resp, int resp_cap) {
    /* Build output path from source path */
    char wasm_path[PATH_MAX];
    snprintf(wasm_path, sizeof(wasm_path), "%s", src_path);
    size_t slen = strlen(wasm_path);
    if (slen > 2 && wasm_path[slen - 2] == '.' && wasm_path[slen - 1] == 'c') {
        wasm_path[slen - 2] = '\0';
        strcat(wasm_path, ".wasm");
    } else {
        strcat(wasm_path, ".wasm");
    }

    /* Build clang command */
    char cmd[PATH_MAX * 2 + 4096];
    int pos = snprintf(cmd, sizeof(cmd),
        "%s --target=wasm32 -nostdlib -O2 "
        "-Wl,--no-entry -Wl,--export-all",
        clang_path);

    for (int i = 0; i < nexports && pos < (int)sizeof(cmd) - 128; i++) {
        pos += snprintf(cmd + pos, sizeof(cmd) - pos,
            " -Wl,--export=%s", exports[i]);
    }

    pos += snprintf(cmd + pos, sizeof(cmd) - pos,
        " -o %s %s 2>&1", wasm_path, src_path);

    /* Run compiler */
    FILE *p = popen(cmd, "r");
    if (!p) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"popen failed\"}");
        return;
    }

    char output[8192] = {0};
    size_t total = 0;
    while (total < sizeof(output) - 1) {
        size_t n = fread(output + total, 1, sizeof(output) - 1 - total, p);
        if (n == 0) break;
        total += n;
    }
    output[total] = '\0';
    int exit_code = pclose(p);
    exit_code = WEXITSTATUS(exit_code);

    if (exit_code != 0) {
        /* Compilation failed — return error */
        char esc_output[16384];
        json_escape(output, (int)total, esc_output, sizeof(esc_output));
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"%s\"}", esc_output);
        unlink(wasm_path);
        return;
    }

    /* Read the .wasm output */
    FILE *wf = fopen(wasm_path, "rb");
    if (!wf) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"cannot read wasm output\"}");
        unlink(wasm_path);
        return;
    }

    fseek(wf, 0, SEEK_END);
    long wasm_size = ftell(wf);
    fseek(wf, 0, SEEK_SET);

    if (wasm_size > MAX_WASM_OUTPUT) {
        fclose(wf);
        unlink(wasm_path);
        snprintf(resp, resp_cap,
            "{\"ok\":false,\"error\":\"wasm output too large (%ld bytes)\"}", wasm_size);
        return;
    }

    unsigned char *wasm_data = malloc(wasm_size);
    if (!wasm_data) {
        fclose(wf);
        unlink(wasm_path);
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"oom\"}");
        return;
    }

    size_t nr = fread(wasm_data, 1, wasm_size, wf);
    fclose(wf);
    unlink(wasm_path);

    /* Base64 encode */
    size_t b64_len = 0;
    char *b64 = base64_encode(wasm_data, nr, &b64_len);
    free(wasm_data);

    if (!b64) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"base64 encode failed\"}");
        return;
    }

    /* Build response */
    snprintf(resp, resp_cap,
        "{\"ok\":true,\"wasm\":\"%s\",\"size\":%ld}", b64, (long)nr);
    free(b64);
}

/* ------------------------------------------------------------------ */
/*  Action handlers                                                     */
/* ------------------------------------------------------------------ */

static void handle_discover(char *resp, int resp_cap) {
    char version[256] = {0};
    get_clang_version(version, sizeof(version));
    char esc_version[512];
    json_escape(version, (int)strlen(version), esc_version, sizeof(esc_version));

    snprintf(resp, resp_cap,
        "{\"ok\":true,\"name\":\"cobbler\",\"actions\":{"
        "\"discover\":{\"params\":{}},"
        "\"compile\":{\"params\":{\"source\":\"string\",\"exports\":\"string[] (optional)\"}},"
        "\"compile_file\":{\"params\":{\"path\":\"string\",\"exports\":\"string[] (optional)\"}}"
        "},\"clang\":\"%s\",\"clang_version\":\"%s\",\"root\":\"%s\"}",
        clang_path, esc_version, root_path);
}

static void handle_compile(const char *req, char *resp, int resp_cap) {
    /* Extract source */
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

    /* Extract optional exports array */
    char *exports[MAX_EXPORTS];
    int nexports = json_get_string_array(req, "exports", exports, MAX_EXPORTS);

    /* Write source to temp file */
    char tmp_src[PATH_MAX];
    snprintf(tmp_src, sizeof(tmp_src), "/tmp/cobbler_XXXXXX.c");
    /* mkstemp needs the template without .c, so use a different approach */
    char tmp_base[PATH_MAX];
    snprintf(tmp_base, sizeof(tmp_base), "/tmp/cobbler_XXXXXX");
    int fd = mkstemp(tmp_base);
    if (fd < 0) {
        free(source);
        for (int i = 0; i < nexports; i++) free(exports[i]);
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"mkstemp failed\"}");
        return;
    }
    close(fd);

    /* Rename to .c */
    snprintf(tmp_src, sizeof(tmp_src), "%s.c", tmp_base);
    rename(tmp_base, tmp_src);

    FILE *f = fopen(tmp_src, "w");
    if (!f) {
        unlink(tmp_src);
        free(source);
        for (int i = 0; i < nexports; i++) free(exports[i]);
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"cannot write temp file\"}");
        return;
    }
    fwrite(source, 1, slen, f);
    fclose(f);
    free(source);

    do_compile(tmp_src, (const char **)exports, nexports, resp, resp_cap);

    /* Clean up */
    unlink(tmp_src);
    for (int i = 0; i < nexports; i++) free(exports[i]);
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

    /* Verify file exists */
    if (access(safe, R_OK) != 0) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"cannot read file\"}");
        return;
    }

    /* Check file size */
    struct stat st;
    if (stat(safe, &st) != 0 || st.st_size > MAX_SOURCE_SIZE) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"file too large\"}");
        return;
    }

    /* Extract optional exports array */
    char *exports[MAX_EXPORTS];
    int nexports = json_get_string_array(req, "exports", exports, MAX_EXPORTS);

    /* Copy to temp file so output goes to /tmp */
    char tmp_base[PATH_MAX];
    snprintf(tmp_base, sizeof(tmp_base), "/tmp/cobbler_XXXXXX");
    int fd = mkstemp(tmp_base);
    if (fd < 0) {
        for (int i = 0; i < nexports; i++) free(exports[i]);
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"mkstemp failed\"}");
        return;
    }
    close(fd);

    char tmp_src[PATH_MAX];
    snprintf(tmp_src, sizeof(tmp_src), "%s.c", tmp_base);
    rename(tmp_base, tmp_src);

    /* Copy source file to temp */
    FILE *sf = fopen(safe, "r");
    FILE *tf = fopen(tmp_src, "w");
    if (!sf || !tf) {
        if (sf) fclose(sf);
        if (tf) fclose(tf);
        unlink(tmp_src);
        for (int i = 0; i < nexports; i++) free(exports[i]);
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"cannot copy source\"}");
        return;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), sf)) > 0)
        fwrite(buf, 1, n, tf);
    fclose(sf);
    fclose(tf);

    do_compile(tmp_src, (const char **)exports, nexports, resp, resp_cap);

    unlink(tmp_src);
    for (int i = 0; i < nexports; i++) free(exports[i]);
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
        /* Unwrap talk command: {"action":"say","message":"{...}"} */
        char msg[8192] = {0};
        json_get_string(req, "message", msg, sizeof(msg));
        if (msg[0] == '{') {
            /* Message is JSON — dispatch it */
            handle_request(msg, (int)strlen(msg), resp, resp_cap);
        } else if (msg[0]) {
            /* Plain text — show help */
            snprintf(resp, resp_cap,
                "{\"ok\":true,\"message\":\"cobbler compiles C to WASM. "
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
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* Resolve root to absolute path */
    if (!realpath(root, root_path)) {
        fprintf(stderr, "cobbler: cannot resolve root '%s'\n", root);
        return 1;
    }

    /* Set clang path if provided */
    if (clang && clang[0])
        snprintf(clang_path, sizeof(clang_path), "%s", clang);

    /* Find and verify clang */
    if (find_clang() != 0) return 1;
    if (verify_wasm_target() != 0) return 1;

    fprintf(stderr, "cobbler: using clang at %s\n", clang_path);

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
    const char *clang = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--endpoint") == 0 && i + 1 < argc)
            endpoint = argv[++i];
        else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc)
            root = argv[++i];
        else if (strcmp(argv[i], "--clang") == 0 && i + 1 < argc)
            clang = argv[++i];
        else {
            fprintf(stderr, "usage: cobbler [--endpoint EP] [--root PATH] [--clang PATH]\n");
            return 1;
        }
    }

    return cobbler_run(endpoint, root, clang);
}
#endif
