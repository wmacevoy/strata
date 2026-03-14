/*
 * anthropic — a vocation den that provides Claude API access.
 *
 * Specialized messenger: owns the API key, handles the Anthropic
 * protocol. Dens just send messages, get responses. No HTTP,
 * no headers, no key management in den code.
 *
 * Actions: discover, ask, models
 *
 * The API key comes from ANTHROPIC_API_KEY env var or --api-key flag.
 * Dens never see it.
 *
 * Usage:
 *   anthropic --endpoint tcp://127.0.0.1:5593
 *   anthropic --endpoint tcp://127.0.0.1:5593 --api-key sk-...
 *   anthropic --endpoint tcp://127.0.0.1:5593 --model claude-sonnet-4-6
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "strata/msg.h"
#include <curl/curl.h>

#include "strata/aead.h"
#include "strata/json_util.h"

#define MAX_REQUEST   (1 * 1024 * 1024)    /* 1MB inbound request */
#define MAX_API_BODY  (4 * 1024 * 1024)    /* 4MB API response body */
#define RESP_CAP      (8 * 1024 * 1024)    /* 8MB outbound response */
#define API_TIMEOUT   120                   /* seconds */

static volatile int running = 1;
static char api_key[256] = "";
static char default_model[64] = "claude-sonnet-4-6";
static int api_timeout = API_TIMEOUT;

static void load_config(const char *path);

static void sigint_handler(int sig) {
    (void)sig;
    running = 0;
}

/* ------------------------------------------------------------------ */
/*  curl write callback                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t size;
    size_t cap;
} write_buf_t;

static size_t write_callback(void *contents, size_t size, size_t nmemb,
                              void *userp) {
    size_t total = size * nmemb;
    write_buf_t *buf = (write_buf_t *)userp;
    if (buf->size + total > MAX_API_BODY) return 0;
    if (buf->size + total >= buf->cap) {
        size_t new_cap = buf->cap * 2;
        if (new_cap < buf->size + total + 1) new_cap = buf->size + total + 1;
        if (new_cap > MAX_API_BODY + 1) new_cap = MAX_API_BODY + 1;
        char *new_data = realloc(buf->data, new_cap);
        if (!new_data) return 0;
        buf->data = new_data;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

/* ------------------------------------------------------------------ */
/*  discover                                                            */
/* ------------------------------------------------------------------ */

static void handle_discover(char *resp, int resp_cap) {
    snprintf(resp, resp_cap,
        "{\"ok\":true,\"name\":\"anthropic\",\"actions\":{"
        "\"discover\":{\"params\":{}},"
        "\"ask\":{\"params\":{"
            "\"messages\":\"array of {role,content}\","
            "\"model\":\"string (optional, default %s)\","
            "\"system\":\"string (optional)\","
            "\"max_tokens\":\"number (optional, default 4096)\","
            "\"tools\":\"array (optional)\""
        "}},"
        "\"models\":{\"params\":{}}"
        "},\"has_key\":%s,\"default_model\":\"%s\"}",
        default_model,
        api_key[0] ? "true" : "false",
        default_model);
}

/* ------------------------------------------------------------------ */
/*  models — list available models                                     */
/* ------------------------------------------------------------------ */

static void handle_models(char *resp, int resp_cap) {
    snprintf(resp, resp_cap,
        "{\"ok\":true,\"models\":["
        "\"claude-opus-4-6\","
        "\"claude-sonnet-4-6\","
        "\"claude-haiku-4-5-20251001\""
        "],\"default\":\"%s\"}", default_model);
}

/* ------------------------------------------------------------------ */
/*  ask — call the Anthropic Messages API                              */
/* ------------------------------------------------------------------ */

static void handle_ask(const char *req, char *resp, int resp_cap) {
    if (!api_key[0]) {
        snprintf(resp, resp_cap,
            "{\"ok\":false,\"error\":\"no API key — set ANTHROPIC_API_KEY or use --api-key\"}");
        return;
    }

    /* Extract model (optional, has default) */
    char model[64];
    if (json_get_string(req, "model", model, sizeof(model)) <= 0)
        strncpy(model, default_model, sizeof(model) - 1);

    /* Extract max_tokens (optional, default 4096) */
    int max_tokens = 4096;
    json_get_int(req, "max_tokens", &max_tokens);

    /*
     * Build the API payload. The request already contains "messages",
     * and optionally "system" and "tools". We reconstruct the API body
     * by wrapping these with model/max_tokens.
     *
     * Strategy: find the "messages" array in the raw JSON and copy it
     * verbatim. Same for "system" and "tools". This avoids parsing
     * and re-serializing complex nested JSON.
     */
    char *api_body = malloc(MAX_REQUEST + 512);
    if (!api_body) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"oom\"}");
        return;
    }

    int pos = 0;
    pos += snprintf(api_body + pos, MAX_REQUEST + 512 - pos,
        "{\"model\":\"%s\",\"max_tokens\":%d", model, max_tokens);

    /* Copy "messages" array verbatim from request */
    const char *msg_start = strstr(req, "\"messages\"");
    if (!msg_start) {
        free(api_body);
        snprintf(resp, resp_cap,
            "{\"ok\":false,\"error\":\"messages array required\"}");
        return;
    }
    /* Find the colon after "messages" */
    const char *colon = strchr(msg_start, ':');
    if (!colon) {
        free(api_body);
        snprintf(resp, resp_cap,
            "{\"ok\":false,\"error\":\"malformed messages\"}");
        return;
    }
    /* Find the opening bracket */
    const char *bracket = colon + 1;
    while (*bracket == ' ' || *bracket == '\t' || *bracket == '\n') bracket++;
    if (*bracket != '[') {
        free(api_body);
        snprintf(resp, resp_cap,
            "{\"ok\":false,\"error\":\"messages must be an array\"}");
        return;
    }
    /* Find matching close bracket */
    int depth = 0;
    const char *p = bracket;
    int in_string = 0;
    while (*p) {
        if (*p == '\\' && in_string) { p++; if (*p) p++; continue; }
        if (*p == '"') in_string = !in_string;
        if (!in_string) {
            if (*p == '[') depth++;
            else if (*p == ']') { depth--; if (depth == 0) { p++; break; } }
        }
        p++;
    }
    int msg_len = (int)(p - bracket);
    pos += snprintf(api_body + pos, MAX_REQUEST + 512 - pos,
        ",\"messages\":%.*s", msg_len, bracket);

    /* Copy "system" string if present */
    char system_buf[8192];
    int sys_len = json_get_string(req, "system", system_buf, sizeof(system_buf));
    if (sys_len > 0) {
        char esc_sys[16384];
        json_escape(system_buf, sys_len, esc_sys, sizeof(esc_sys));
        pos += snprintf(api_body + pos, MAX_REQUEST + 512 - pos,
            ",\"system\":\"%s\"", esc_sys);
    }

    /* Copy "tools" array if present */
    const char *tools_start = strstr(req, "\"tools\"");
    if (tools_start) {
        const char *tc = strchr(tools_start, ':');
        if (tc) {
            const char *tb = tc + 1;
            while (*tb == ' ' || *tb == '\t' || *tb == '\n') tb++;
            if (*tb == '[') {
                int td = 0;
                const char *tp = tb;
                int tin = 0;
                while (*tp) {
                    if (*tp == '\\' && tin) { tp++; if (*tp) tp++; continue; }
                    if (*tp == '"') tin = !tin;
                    if (!tin) {
                        if (*tp == '[') td++;
                        else if (*tp == ']') { td--; if (td == 0) { tp++; break; } }
                    }
                    tp++;
                }
                int tlen = (int)(tp - tb);
                pos += snprintf(api_body + pos, MAX_REQUEST + 512 - pos,
                    ",\"tools\":%.*s", tlen, tb);
            }
        }
    }

    pos += snprintf(api_body + pos, MAX_REQUEST + 512 - pos, "}");

    /* Make the API call */
    CURL *curl = curl_easy_init();
    if (!curl) {
        free(api_body);
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"curl init failed\"}");
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, api_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(api_body));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)api_timeout);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    char auth_header[300];
    snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", api_key);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "content-type: application/json");
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    write_buf_t wbuf = { .data = malloc(4096), .size = 0, .cap = 4096 };
    if (!wbuf.data) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(api_body);
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"oom\"}");
        return;
    }
    wbuf.data[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wbuf);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        const char *err = curl_easy_strerror(res);
        char esc_err[512];
        json_escape(err, (int)strlen(err), esc_err, sizeof(esc_err));
        snprintf(resp, resp_cap,
            "{\"ok\":false,\"error\":\"curl: %s\"}", esc_err);
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code == 200) {
            /* Success — return the API response directly.
             * The body is already valid JSON from the API.
             * We wrap it minimally. */
            size_t esc_cap = wbuf.size * 2 + 1;
            if (esc_cap > (size_t)resp_cap - 256) esc_cap = (size_t)resp_cap - 256;
            char *escaped = malloc(esc_cap);
            if (escaped) {
                int elen = json_escape(wbuf.data, (int)wbuf.size,
                                        escaped, (int)esc_cap);
                snprintf(resp, resp_cap,
                    "{\"ok\":true,\"status\":200,\"body\":\"%.*s\"}",
                    elen, escaped);
                free(escaped);
            } else {
                snprintf(resp, resp_cap,
                    "{\"ok\":false,\"error\":\"oom escaping response\"}");
            }
        } else {
            /* Error — include status and body */
            size_t esc_cap = wbuf.size * 2 + 1;
            if (esc_cap > 4096) esc_cap = 4096;
            char *escaped = malloc(esc_cap);
            if (escaped) {
                int elen = json_escape(wbuf.data, (int)wbuf.size,
                                        escaped, (int)esc_cap);
                snprintf(resp, resp_cap,
                    "{\"ok\":false,\"status\":%ld,\"error\":\"%.*s\"}",
                    http_code, elen, escaped);
                free(escaped);
            } else {
                snprintf(resp, resp_cap,
                    "{\"ok\":false,\"status\":%ld,\"error\":\"API error\"}", http_code);
            }
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(wbuf.data);
    free(api_body);
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
        snprintf(resp, resp_cap,
            "{\"ok\":true,\"name\":\"anthropic\",\"has_key\":%s}",
            api_key[0] ? "true" : "false");
    } else if (strcmp(action, "say") == 0) {
        /* Plain text via talk → treat as single-turn ask */
        char msg[8192] = {0};
        json_get_string(req, "message", msg, sizeof(msg));
        if (msg[0] == '{') {
            /* JSON inside say — dispatch normally */
            handle_request(msg, (int)strlen(msg), resp, resp_cap);
        } else if (msg[0]) {
            /* Wrap in a simple ask */
            char wrapped[16384];
            char esc_msg[8192];
            json_escape(msg, (int)strlen(msg), esc_msg, sizeof(esc_msg));
            snprintf(wrapped, sizeof(wrapped),
                "{\"action\":\"ask\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
                esc_msg);
            handle_request(wrapped, (int)strlen(wrapped), resp, resp_cap);
        } else {
            handle_discover(resp, resp_cap);
        }
    } else if (strcmp(action, "discover") == 0)
        handle_discover(resp, resp_cap);
    else if (strcmp(action, "ask") == 0)
        handle_ask(req, resp, resp_cap);
    else if (strcmp(action, "models") == 0)
        handle_models(resp, resp_cap);
    else
        snprintf(resp, resp_cap,
            "{\"ok\":false,\"error\":\"unknown action: %s\"}", action);
}

/* ------------------------------------------------------------------ */
/*  Service main loop                                                   */
/* ------------------------------------------------------------------ */

int anthropic_run(const char *endpoint, int timeout) {
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    if (timeout > 0) api_timeout = timeout;

    /* Check env and config if key not already set */
    if (!api_key[0]) {
        const char *env_key = getenv("ANTHROPIC_API_KEY");
        if (env_key) strncpy(api_key, env_key, sizeof(api_key) - 1);
    }
    if (!api_key[0]) {
        load_config("private/config.json");
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    strata_sock *listener = strata_rep_bind(endpoint);
    if (!listener) {
        fprintf(stderr, "anthropic: cannot bind to %s\n", endpoint);
        curl_global_cleanup();
        return 1;
    }
    strata_msg_set_timeout(listener, 1000, -1);

    fprintf(stderr, "anthropic: listening on %s  key=%s  model=%s  timeout=%ds\n",
            endpoint,
            api_key[0] ? "yes" : "NO",
            default_model,
            api_timeout);

    char *req_buf = malloc(MAX_REQUEST + 4096);
    char *resp_buf = malloc(RESP_CAP);
    if (!req_buf || !resp_buf) {
        fprintf(stderr, "anthropic: malloc failed\n");
        free(req_buf); free(resp_buf);
        strata_sock_close(listener);
        curl_global_cleanup();
        return 1;
    }

    while (running) {
        strata_sock *client = strata_rep_accept(listener);
        if (!client) continue;

        int rc = strata_recv(client, req_buf, MAX_REQUEST + 4095, 0);
        if (rc < 0) { strata_sock_close(client); continue; }
        req_buf[rc] = '\0';

        resp_buf[0] = '\0';
        handle_request(req_buf, rc, resp_buf, RESP_CAP);
        strata_send(client, resp_buf, strlen(resp_buf), 0);
        strata_sock_close(client);
    }

    free(req_buf);
    free(resp_buf);
    strata_sock_close(listener);
    curl_global_cleanup();
    fprintf(stderr, "anthropic: shutdown\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Standalone entry point                                              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Load API key from config file                                       */
/*  Reads "key" from {"strata":{"key":"..."}} in a JSON file.           */
/* ------------------------------------------------------------------ */

static void load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return;
    buf[n] = '\0';

    /* Find the "anthropic" section, then extract "key" within it.
     * json_get_string expects "key":"val" (no spaces around colon),
     * so we search manually to handle "key" : "val" formatting. */
    const char *section = strstr(buf, "\"anthropic\"");
    if (!section) return;
    const char *brace = strchr(section, '{');
    if (!brace) return;

    /* Find "key" within the anthropic section */
    const char *kp = strstr(brace, "\"key\"");
    if (!kp) return;
    /* Skip past "key", whitespace, colon, whitespace, opening quote */
    kp += 5; /* past "key" */
    while (*kp == ' ' || *kp == '\t' || *kp == '\n' || *kp == '\r') kp++;
    if (*kp != ':') return;
    kp++;
    while (*kp == ' ' || *kp == '\t' || *kp == '\n' || *kp == '\r') kp++;
    if (*kp != '"') return;
    kp++; /* past opening quote */

    /* Copy value until closing quote */
    int pos = 0;
    while (*kp && *kp != '"' && pos < (int)sizeof(api_key) - 1) {
        if (*kp == '\\' && *(kp + 1)) { kp++; }
        api_key[pos++] = *kp++;
    }
    api_key[pos] = '\0';
}

#ifndef ANTHROPIC_NO_MAIN
int main(int argc, char **argv) {
    const char *endpoint = "tcp://127.0.0.1:5593";
    const char *config_path = "private/config.json";
    int timeout = API_TIMEOUT;

    /* Check env first */
    const char *env_key = getenv("ANTHROPIC_API_KEY");
    if (env_key)
        strncpy(api_key, env_key, sizeof(api_key) - 1);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--endpoint") == 0 && i + 1 < argc)
            endpoint = argv[++i];
        else if (strcmp(argv[i], "--api-key") == 0 && i + 1 < argc)
            strncpy(api_key, argv[++i], sizeof(api_key) - 1);
        else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
            strncpy(default_model, argv[++i], sizeof(default_model) - 1);
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
            timeout = atoi(argv[++i]);
        else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            config_path = argv[++i];
        else {
            fprintf(stderr,
                "usage: anthropic [--endpoint EP] [--api-key KEY] "
                "[--model MODEL] [--timeout SECS] [--config PATH]\n");
            return 1;
        }
    }

    /* Config file is lowest priority: env > flag > config */
    if (!api_key[0])
        load_config(config_path);

    return anthropic_run(endpoint, timeout);
}
#endif
