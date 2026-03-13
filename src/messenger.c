/*
 * messenger — a vocation den that provides HTTP client capabilities.
 *
 * JSON-over-TCP-REP service. Same pattern as code-smith.
 *
 * Actions: discover, fetch
 *
 * Usage:
 *   messenger --endpoint tcp://127.0.0.1:5592
 *   messenger --endpoint tcp://127.0.0.1:5592 --timeout 120
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

#define MAX_REQUEST_BODY   (1 * 1024 * 1024)   /* 1MB request body */
#define MAX_RESPONSE_BODY  (4 * 1024 * 1024)   /* 4MB response body */
#define RESP_CAP           (8 * 1024 * 1024)   /* 8MB response */
#define DEFAULT_TIMEOUT    120                  /* seconds */
#define MAX_HEADERS        32

static volatile int running = 1;
static int http_timeout = DEFAULT_TIMEOUT;

static void sigint_handler(int sig) {
    (void)sig;
    running = 0;
}

/* ------------------------------------------------------------------ */
/*  curl write callback — accumulate into dynamic buffer               */
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
    if (buf->size + total > MAX_RESPONSE_BODY)
        return 0; /* abort — too large */
    if (buf->size + total >= buf->cap) {
        size_t new_cap = buf->cap * 2;
        if (new_cap < buf->size + total + 1) new_cap = buf->size + total + 1;
        if (new_cap > MAX_RESPONSE_BODY + 1) new_cap = MAX_RESPONSE_BODY + 1;
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
/*  Action handlers                                                     */
/* ------------------------------------------------------------------ */

static void handle_discover(char *resp, int resp_cap) {
    snprintf(resp, resp_cap,
        "{\"ok\":true,\"name\":\"messenger\",\"actions\":{"
        "\"discover\":{\"params\":{}},"
        "\"fetch\":{\"params\":{"
            "\"url\":\"string\","
            "\"method\":\"string (optional, default GET)\","
            "\"headers\":\"string[] (optional)\","
            "\"body\":\"string (optional)\""
        "}}"
        "},\"timeout\":%d}", http_timeout);
}

static void handle_fetch(const char *req, char *resp, int resp_cap) {
    char url[4096] = {0};
    json_get_string(req, "url", url, sizeof(url));
    if (!url[0]) {
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"url required\"}");
        return;
    }

    char method[16] = "GET";
    json_get_string(req, "method", method, sizeof(method));

    /* Extract optional headers array */
    char *headers[MAX_HEADERS];
    int nheaders = json_get_string_array(req, "headers", headers, MAX_HEADERS);

    /* Extract optional body */
    char *body = malloc(MAX_REQUEST_BODY + 1);
    int body_len = -1;
    if (body) {
        body_len = json_get_string(req, "body", body, MAX_REQUEST_BODY);
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(body);
        for (int i = 0; i < nheaders; i++) free(headers[i]);
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"curl_easy_init failed\"}");
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)http_timeout);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);

    /* Method */
    if (strcmp(method, "POST") == 0)
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    else if (strcmp(method, "PUT") == 0)
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    else if (strcmp(method, "DELETE") == 0)
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    else if (strcmp(method, "PATCH") == 0)
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");

    /* Headers */
    struct curl_slist *header_list = NULL;
    for (int i = 0; i < nheaders; i++) {
        header_list = curl_slist_append(header_list, headers[i]);
        free(headers[i]);
    }
    if (header_list)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    /* Body */
    if (body_len > 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    }

    /* Response buffer */
    write_buf_t wbuf = { .data = malloc(4096), .size = 0, .cap = 4096 };
    if (!wbuf.data) {
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        free(body);
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"oom\"}");
        return;
    }
    wbuf.data[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wbuf);

    /* Perform */
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

        /* Escape response body for JSON */
        size_t esc_cap = wbuf.size * 2 + 1;
        if (esc_cap > (size_t)resp_cap - 256) esc_cap = (size_t)resp_cap - 256;
        char *escaped = malloc(esc_cap);
        if (!escaped) {
            snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"oom escaping response\"}");
        } else {
            int elen = json_escape(wbuf.data, (int)wbuf.size, escaped, (int)esc_cap);
            snprintf(resp, resp_cap,
                "{\"ok\":true,\"status\":%ld,\"size\":%zu,\"body\":\"%.*s\"}",
                http_code, wbuf.size, elen, escaped);
            free(escaped);
        }
    }

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    free(wbuf.data);
    free(body);
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
        snprintf(resp, resp_cap, "{\"ok\":true,\"name\":\"messenger\"}");
    } else if (strcmp(action, "say") == 0) {
        char msg[8192] = {0};
        json_get_string(req, "message", msg, sizeof(msg));
        if (msg[0] == '{') {
            handle_request(msg, (int)strlen(msg), resp, resp_cap);
        } else if (msg[0]) {
            snprintf(resp, resp_cap,
                "{\"ok\":true,\"message\":\"messenger provides HTTP fetch. "
                "Send {\\\"action\\\":\\\"fetch\\\",\\\"url\\\":\\\"...\\\"}  "
                "or {\\\"action\\\":\\\"discover\\\"} for details.\"}");
        } else {
            handle_discover(resp, resp_cap);
        }
    } else if (strcmp(action, "discover") == 0)
        handle_discover(resp, resp_cap);
    else if (strcmp(action, "fetch") == 0)
        handle_fetch(req, resp, resp_cap);
    else
        snprintf(resp, resp_cap, "{\"ok\":false,\"error\":\"unknown action: %s\"}", action);
}

/* ------------------------------------------------------------------ */
/*  Service main loop                                                   */
/* ------------------------------------------------------------------ */

int messenger_run(const char *endpoint, int timeout) {
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    if (timeout > 0) http_timeout = timeout;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    strata_sock *listener = strata_rep_bind(endpoint);
    if (!listener) {
        fprintf(stderr, "messenger: cannot bind to %s\n", endpoint);
        curl_global_cleanup();
        return 1;
    }
    strata_msg_set_timeout(listener, 1000, -1);

    fprintf(stderr, "messenger: listening on %s  timeout=%ds\n",
            endpoint, http_timeout);

    char *req_buf = malloc(MAX_REQUEST_BODY + 4096);
    char *resp_buf = malloc(RESP_CAP);
    if (!req_buf || !resp_buf) {
        fprintf(stderr, "messenger: malloc failed\n");
        free(req_buf); free(resp_buf);
        strata_sock_close(listener);
        curl_global_cleanup();
        return 1;
    }

    while (running) {
        strata_sock *client = strata_rep_accept(listener);
        if (!client) continue;

        int rc = strata_recv(client, req_buf, MAX_REQUEST_BODY + 4095, 0);
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
    fprintf(stderr, "messenger: shutdown\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Standalone entry point                                              */
/* ------------------------------------------------------------------ */

#ifndef MESSENGER_NO_MAIN
int main(int argc, char **argv) {
    const char *endpoint = "tcp://127.0.0.1:5592";
    int timeout = DEFAULT_TIMEOUT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--endpoint") == 0 && i + 1 < argc)
            endpoint = argv[++i];
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
            timeout = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: messenger [--endpoint EP] [--timeout SECS]\n");
            return 1;
        }
    }

    return messenger_run(endpoint, timeout);
}
#endif
