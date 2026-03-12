/*
 * code-smith — a vocation den that provides file I/O and shell tools.
 *
 * JSON-over-ZMQ-REP service. Same pattern as store_service.
 *
 * Actions: discover, read, write, exec, glob, grep, ls
 *
 * Usage:
 *   code_smith --endpoint tcp://127.0.0.1:5590 --root /path/to/project
 *   code_smith --endpoint tcp://127.0.0.1:5590 --root . --readonly
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <glob.h>
#include <zmq.h>

#include "strata/aead.h"
#include "strata/json_util.h"

#define MAX_FILE_SIZE   (1024 * 1024)   /* 1 MB read cap */
#define MAX_EXEC_OUTPUT (64 * 1024)     /* 64 KB exec cap */
#define MAX_RESULTS     1000            /* glob/grep/ls cap */
#define RESP_CAP        (2 * 1024 * 1024)  /* 2 MB response buffer */

static volatile int running = 1;
static char root_path[PATH_MAX] = ".";
static int readonly_mode = 0;

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
/*  Action handlers                                                     */
/* ------------------------------------------------------------------ */

static void handle_discover(char *resp, int resp_cap) {
    snprintf(resp, resp_cap,
        "{\"ok\":true,\"name\":\"code-smith\",\"actions\":{"
        "\"discover\":{\"params\":{}},"
        "\"read\":{\"params\":{\"path\":\"string\"}},"
        "\"write\":{\"params\":{\"path\":\"string\",\"content\":\"string\"}},"
        "\"exec\":{\"params\":{\"cmd\":\"string\"}},"
        "\"glob\":{\"params\":{\"pattern\":\"string\",\"path\":\"string (optional)\"}},"
        "\"grep\":{\"params\":{\"pattern\":\"string\",\"path\":\"string (optional)\"}},"
        "\"ls\":{\"params\":{\"path\":\"string (optional)\"}}"
        "},\"root\":\"%s\",\"readonly\":%s}",
        root_path, readonly_mode ? "true" : "false");
}

static void handle_read(const char *req, char *resp, int resp_cap) {
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

    FILE *f = fopen(safe, "r");
    if (!f) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"cannot open file\"}");
        return;
    }

    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen > MAX_FILE_SIZE) {
        fclose(f);
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"file too large (%ld bytes)\"}",
                 flen);
        return;
    }

    char *content = malloc(flen + 1);
    if (!content) { fclose(f); snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"oom\"}"); return; }
    size_t nr = fread(content, 1, flen, f);
    fclose(f);
    content[nr] = '\0';

    /* Escape content for JSON */
    size_t esc_cap = nr * 2 + 1;
    if (esc_cap > (size_t)resp_cap - 256) esc_cap = (size_t)resp_cap - 256;
    char *escaped = malloc(esc_cap);
    if (!escaped) { free(content); snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"oom\"}"); return; }

    int elen = json_escape(content, (int)nr, escaped, (int)esc_cap);
    free(content);

    snprintf(resp, resp_cap, "{\"ok\":true,\"path\":\"%s\",\"size\":%ld,\"content\":\"%.*s\"}",
             path, flen, elen, escaped);
    free(escaped);
}

static void handle_write(const char *req, char *resp, int resp_cap) {
    if (readonly_mode) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"readonly mode\"}");
        return;
    }

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

    /* Extract content — can be large */
    size_t content_cap = MAX_FILE_SIZE + 1;
    char *content = malloc(content_cap);
    if (!content) { snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"oom\"}"); return; }
    int clen = json_get_string(req, "content", content, (int)content_cap);
    if (clen < 0) {
        free(content);
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"content required\"}");
        return;
    }

    FILE *f = fopen(safe, "w");
    if (!f) {
        free(content);
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"cannot write file\"}");
        return;
    }
    fwrite(content, 1, clen, f);
    fclose(f);
    free(content);

    snprintf(resp, resp_cap, "{\"ok\":true,\"path\":\"%s\",\"bytes\":%d}", path, clen);
}

static void handle_exec(const char *req, char *resp, int resp_cap) {
    if (readonly_mode) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"readonly mode\"}");
        return;
    }

    char cmd[4096] = {0};
    json_get_string(req, "cmd", cmd, sizeof(cmd));
    if (!cmd[0]) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"cmd required\"}");
        return;
    }

    /* Run from root directory */
    char full_cmd[PATH_MAX + 4096 + 16];
    snprintf(full_cmd, sizeof(full_cmd), "cd '%s' && %s 2>&1", root_path, cmd);

    FILE *p = popen(full_cmd, "r");
    if (!p) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"popen failed\"}");
        return;
    }

    char *output = malloc(MAX_EXEC_OUTPUT + 1);
    if (!output) { pclose(p); snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"oom\"}"); return; }

    size_t total = 0;
    while (total < MAX_EXEC_OUTPUT) {
        size_t n = fread(output + total, 1, MAX_EXEC_OUTPUT - total, p);
        if (n == 0) break;
        total += n;
    }
    output[total] = '\0';
    int exit_code = pclose(p);
    exit_code = WEXITSTATUS(exit_code);

    size_t esc_cap = total * 2 + 1;
    if (esc_cap > (size_t)resp_cap - 256) esc_cap = (size_t)resp_cap - 256;
    char *escaped = malloc(esc_cap);
    if (!escaped) { free(output); snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"oom\"}"); return; }

    int elen = json_escape(output, (int)total, escaped, (int)esc_cap);
    free(output);

    snprintf(resp, resp_cap,
        "{\"ok\":true,\"exit_code\":%d,\"stdout\":\"%.*s\"}",
        exit_code, elen, escaped);
    free(escaped);
}

static void handle_glob(const char *req, char *resp, int resp_cap) {
    char pattern[512] = {0};
    char path[256] = {0};
    json_get_string(req, "pattern", pattern, sizeof(pattern));
    json_get_string(req, "path", path, sizeof(path));

    if (!pattern[0]) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"pattern required\"}");
        return;
    }

    char safe_dir[PATH_MAX];
    if (safe_path(path[0] ? path : ".", safe_dir, sizeof(safe_dir)) != 0) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"path not allowed\"}");
        return;
    }

    char full_pattern[PATH_MAX + 512];
    snprintf(full_pattern, sizeof(full_pattern), "%s/%s", safe_dir, pattern);

    glob_t g;
    int rc = glob(full_pattern, GLOB_NOSORT, NULL, &g);
    if (rc != 0 && rc != GLOB_NOMATCH) {
        snprintf(resp, resp_cap, "{\"ok\":true,\"files\":[]}");
        return;
    }

    int pos = 0;
    pos += snprintf(resp + pos, resp_cap - pos, "{\"ok\":true,\"files\":[");

    size_t root_len = strlen(root_path);
    int count = 0;
    for (size_t i = 0; i < g.gl_pathc && count < MAX_RESULTS; i++) {
        const char *p = g.gl_pathv[i];
        /* Strip root prefix for relative paths */
        if (strncmp(p, root_path, root_len) == 0 && p[root_len] == '/')
            p += root_len + 1;

        if (count > 0) pos += snprintf(resp + pos, resp_cap - pos, ",");
        pos += snprintf(resp + pos, resp_cap - pos, "\"%s\"", p);
        count++;
        if (pos >= resp_cap - 64) break;
    }
    pos += snprintf(resp + pos, resp_cap - pos, "],\"count\":%d}", count);
    globfree(&g);
}

static void handle_grep(const char *req, char *resp, int resp_cap) {
    char pattern[512] = {0};
    char path[256] = {0};
    json_get_string(req, "pattern", pattern, sizeof(pattern));
    json_get_string(req, "path", path, sizeof(path));

    if (!pattern[0]) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"pattern required\"}");
        return;
    }

    char safe_dir[PATH_MAX];
    if (safe_path(path[0] ? path : ".", safe_dir, sizeof(safe_dir)) != 0) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"path not allowed\"}");
        return;
    }

    /* Use grep -rn via popen */
    char cmd[PATH_MAX + 1024];
    /* Escape single quotes in pattern */
    char esc_pattern[1024];
    int ep = 0;
    for (int i = 0; pattern[i] && ep < (int)sizeof(esc_pattern) - 4; i++) {
        if (pattern[i] == '\'') {
            esc_pattern[ep++] = '\'';
            esc_pattern[ep++] = '\\';
            esc_pattern[ep++] = '\'';
            esc_pattern[ep++] = '\'';
        } else {
            esc_pattern[ep++] = pattern[i];
        }
    }
    esc_pattern[ep] = '\0';

    snprintf(cmd, sizeof(cmd), "grep -rn '%s' '%s' 2>/dev/null | head -n %d",
             esc_pattern, safe_dir, MAX_RESULTS);

    FILE *p = popen(cmd, "r");
    if (!p) {
        snprintf(resp, resp_cap, "{\"ok\":true,\"matches\":[]}");
        return;
    }

    int pos = 0;
    pos += snprintf(resp + pos, resp_cap - pos, "{\"ok\":true,\"matches\":[");

    char line[4096];
    int count = 0;
    size_t root_len = strlen(root_path);
    while (fgets(line, sizeof(line), p) && count < MAX_RESULTS) {
        /* Format: file:line:text */
        char *colon1 = strchr(line, ':');
        if (!colon1) continue;
        char *colon2 = strchr(colon1 + 1, ':');
        if (!colon2) continue;

        *colon1 = '\0';
        *colon2 = '\0';
        const char *file = line;
        const char *linenum = colon1 + 1;
        char *text = colon2 + 1;

        /* Strip trailing newline */
        size_t tlen = strlen(text);
        if (tlen > 0 && text[tlen - 1] == '\n') text[--tlen] = '\0';

        /* Strip root prefix */
        if (strncmp(file, root_path, root_len) == 0 && file[root_len] == '/')
            file += root_len + 1;

        /* Escape text for JSON */
        char esc_text[2048];
        json_escape(text, (int)tlen, esc_text, sizeof(esc_text));

        if (count > 0) pos += snprintf(resp + pos, resp_cap - pos, ",");
        pos += snprintf(resp + pos, resp_cap - pos,
            "{\"file\":\"%s\",\"line\":%s,\"text\":\"%s\"}",
            file, linenum, esc_text);
        count++;
        if (pos >= resp_cap - 256) break;
    }
    pclose(p);

    pos += snprintf(resp + pos, resp_cap - pos, "],\"count\":%d}", count);
}

static void handle_ls(const char *req, char *resp, int resp_cap) {
    char path[256] = {0};
    json_get_string(req, "path", path, sizeof(path));

    char safe[PATH_MAX];
    if (safe_path(path[0] ? path : ".", safe, sizeof(safe)) != 0) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"path not allowed\"}");
        return;
    }

    DIR *d = opendir(safe);
    if (!d) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"cannot open directory\"}");
        return;
    }

    int pos = 0;
    pos += snprintf(resp + pos, resp_cap - pos, "{\"ok\":true,\"entries\":[");

    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(d)) && count < MAX_RESULTS) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        /* Get file info */
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", safe, ent->d_name);
        struct stat st;
        const char *type = "file";
        if (stat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) type = "dir";
            else if (S_ISLNK(st.st_mode)) type = "link";
        }

        if (count > 0) pos += snprintf(resp + pos, resp_cap - pos, ",");
        pos += snprintf(resp + pos, resp_cap - pos,
            "{\"name\":\"%s\",\"type\":\"%s\"}", ent->d_name, type);
        count++;
        if (pos >= resp_cap - 128) break;
    }
    closedir(d);

    pos += snprintf(resp + pos, resp_cap - pos, "],\"count\":%d}", count);
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
        snprintf(resp, resp_cap, "{\"ok\":true,\"name\":\"code-smith\"}");
    } else if (strcmp(action, "say") == 0) {
        /* Unwrap talk command: {"action":"say","message":"{...}"} */
        char msg[8192] = {0};
        json_get_string(req, "message", msg, sizeof(msg));
        if (msg[0] == '{') {
            /* Message is JSON — dispatch it */
            handle_request(msg, (int)strlen(msg), resp, resp_cap);
        } else {
            /* Plain text — treat as exec if not readonly */
            if (readonly_mode) {
                snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"readonly mode\"}");
            } else if (msg[0]) {
                /* Build an exec request from the plain text */
                char exec_req[8192];
                char esc_msg[4096];
                json_escape(msg, (int)strlen(msg), esc_msg, sizeof(esc_msg));
                snprintf(exec_req, sizeof(exec_req),
                    "{\"action\":\"exec\",\"cmd\":\"%s\"}", esc_msg);
                handle_request(exec_req, (int)strlen(exec_req), resp, resp_cap);
            } else {
                handle_discover(resp, resp_cap);
            }
        }
    } else if (strcmp(action, "discover") == 0)
        handle_discover(resp, resp_cap);
    else if (strcmp(action, "read") == 0)
        handle_read(req, resp, resp_cap);
    else if (strcmp(action, "write") == 0)
        handle_write(req, resp, resp_cap);
    else if (strcmp(action, "exec") == 0)
        handle_exec(req, resp, resp_cap);
    else if (strcmp(action, "glob") == 0)
        handle_glob(req, resp, resp_cap);
    else if (strcmp(action, "grep") == 0)
        handle_grep(req, resp, resp_cap);
    else if (strcmp(action, "ls") == 0)
        handle_ls(req, resp, resp_cap);
    else
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"unknown action: %s\"}", action);
}

/* ------------------------------------------------------------------ */
/*  Service main loop                                                   */
/* ------------------------------------------------------------------ */

int code_smith_run(const char *endpoint, const char *root, int readonly) {
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* Resolve root to absolute path */
    if (!realpath(root, root_path)) {
        fprintf(stderr, "code-smith: cannot resolve root '%s'\n", root);
        return 1;
    }
    readonly_mode = readonly;

    void *zmq_ctx = zmq_ctx_new();
    void *rep = zmq_socket(zmq_ctx, ZMQ_REP);
    zmq_bind(rep, endpoint);

    int timeout = 1000;
    zmq_setsockopt(rep, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));

    fprintf(stderr, "code-smith: listening on %s  root=%s  readonly=%d\n",
            endpoint, root_path, readonly_mode);

    char *req_buf = malloc(MAX_FILE_SIZE + 4096);
    char *resp_buf = malloc(RESP_CAP);
    if (!req_buf || !resp_buf) {
        fprintf(stderr, "code-smith: malloc failed\n");
        free(req_buf); free(resp_buf);
        zmq_close(rep); zmq_ctx_destroy(zmq_ctx);
        return 1;
    }

    while (running) {
        int rc = strata_zmq_recv(rep, req_buf, MAX_FILE_SIZE + 4095, 0);
        if (rc < 0) continue;
        req_buf[rc] = '\0';

        resp_buf[0] = '\0';
        handle_request(req_buf, rc, resp_buf, RESP_CAP);
        strata_zmq_send(rep, resp_buf, strlen(resp_buf), 0);
    }

    free(req_buf);
    free(resp_buf);
    zmq_close(rep);
    zmq_ctx_destroy(zmq_ctx);
    fprintf(stderr, "code-smith: shutdown\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Standalone entry point                                              */
/* ------------------------------------------------------------------ */

#ifndef CODE_SMITH_NO_MAIN
int main(int argc, char **argv) {
    const char *endpoint = "tcp://127.0.0.1:5590";
    const char *root = ".";
    int readonly = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--endpoint") == 0 && i + 1 < argc)
            endpoint = argv[++i];
        else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc)
            root = argv[++i];
        else if (strcmp(argv[i], "--readonly") == 0)
            readonly = 1;
        else {
            fprintf(stderr, "usage: code_smith [--endpoint EP] [--root PATH] [--readonly]\n");
            return 1;
        }
    }

    return code_smith_run(endpoint, root, readonly);
}
#endif
