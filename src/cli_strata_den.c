/*
 * strata-den CLI — den in container.
 *
 * All operations via ZMQ REQ/REP to store_service:
 *   msg post/list/get, blob put/get/find/tag/untag/tags, listen.
 *
 * No admin commands — bedrock enforces safety.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <zmq.h>
#include "strata/aead.h"

/* ------------------------------------------------------------------ */
/*  Options                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *endpoint;
    const char *entity;
    const char *roles_csv;
    const char *tags_csv;
    const char *type_filter;
    const char *file_path;
    const char *topic;
    int plain;
    int timeout_ms;
    int argc;
    char **argv;
} den_opts;

/* ------------------------------------------------------------------ */
/*  CSV parsing                                                        */
/* ------------------------------------------------------------------ */

static int parse_csv(const char *csv, char **out, int max) {
    if (!csv || !csv[0]) return 0;
    char *copy = strdup(csv);
    int count = 0;
    char *tok = strtok(copy, ",");
    while (tok && count < max) {
        out[count++] = strdup(tok);
        tok = strtok(NULL, ",");
    }
    free(copy);
    return count;
}

static void free_csv(char **arr, int count) {
    for (int i = 0; i < count; i++) free(arr[i]);
}

/* ------------------------------------------------------------------ */
/*  ZMQ helpers                                                        */
/* ------------------------------------------------------------------ */

static int zmq_do_request(void *req_sock, const char *request,
                          char *resp, int resp_cap) {
    int rc = strata_zmq_send(req_sock, request, strlen(request), 0);
    if (rc < 0) return -1;
    rc = strata_zmq_recv(req_sock, resp, resp_cap - 1, 0);
    if (rc < 0) return -1;
    resp[rc] = '\0';
    return rc;
}

/* Build a JSON roles array fragment: "role1","role2" */
static int append_json_array(char *buf, int cap, int pos,
                              const char *key, char **items, int count) {
    pos += snprintf(buf + pos, cap - pos, "\"%s\":[", key);
    for (int i = 0; i < count; i++)
        pos += snprintf(buf + pos, cap - pos, "%s\"%s\"", i > 0 ? "," : "", items[i]);
    pos += snprintf(buf + pos, cap - pos, "]");
    return pos;
}

/* ------------------------------------------------------------------ */
/*  Artifact commands                                                  */
/* ------------------------------------------------------------------ */

static int cmd_msg_post(void *sock, den_opts *opts) {
    if (opts->argc < 3) {
        fprintf(stderr, "usage: strata-den ... msg post <repo> <type> <content> --roles r1,r2\n");
        return 1;
    }
    char *roles[16];
    int nroles = parse_csv(opts->roles_csv, roles, 16);
    if (nroles == 0) { fprintf(stderr, "--roles required\n"); return 1; }

    char req[8192];
    int pos = snprintf(req, sizeof(req),
        "{\"action\":\"put\",\"repo\":\"%s\",\"type\":\"%s\","
        "\"content\":\"%s\",\"author\":\"%s\",",
        opts->argv[0], opts->argv[1], opts->argv[2], opts->entity);
    pos = append_json_array(req, sizeof(req), pos, "roles", roles, nroles);
    snprintf(req + pos, sizeof(req) - pos, "}");
    free_csv(roles, nroles);

    char resp[8192];
    if (zmq_do_request(sock, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "{\"ok\":false,\"error\":\"timeout\"}\n");
        return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_msg_list(void *sock, den_opts *opts) {
    if (opts->argc < 1) {
        fprintf(stderr, "usage: strata-den ... msg list <repo> [--type <type>]\n");
        return 1;
    }
    char req[2048];
    if (opts->type_filter && opts->type_filter[0])
        snprintf(req, sizeof(req),
            "{\"action\":\"list\",\"repo\":\"%s\",\"type\":\"%s\",\"entity\":\"%s\"}",
            opts->argv[0], opts->type_filter, opts->entity);
    else
        snprintf(req, sizeof(req),
            "{\"action\":\"list\",\"repo\":\"%s\",\"entity\":\"%s\"}",
            opts->argv[0], opts->entity);

    char resp[16384];
    if (zmq_do_request(sock, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "{\"ok\":false,\"error\":\"timeout\"}\n");
        return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_msg_get(void *sock, den_opts *opts) {
    if (opts->argc < 1) {
        fprintf(stderr, "usage: strata-den ... msg get <artifact_id>\n");
        return 1;
    }
    char req[512];
    snprintf(req, sizeof(req),
        "{\"action\":\"get\",\"id\":\"%s\",\"entity\":\"%s\"}",
        opts->argv[0], opts->entity);

    char resp[8192];
    if (zmq_do_request(sock, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "{\"ok\":false,\"error\":\"timeout\"}\n");
        return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/*  Blob commands                                                      */
/* ------------------------------------------------------------------ */

static int cmd_blob_put(void *sock, den_opts *opts) {
    const char *content = NULL;
    char *file_content = NULL;

    if (opts->file_path) {
        FILE *f = fopen(opts->file_path, "rb");
        if (!f) { fprintf(stderr, "cannot open file: %s\n", opts->file_path); return 1; }
        fseek(f, 0, SEEK_END);
        long flen = ftell(f);
        fseek(f, 0, SEEK_SET);
        file_content = malloc(flen + 1);
        fread(file_content, 1, flen, f);
        file_content[flen] = '\0';
        fclose(f);
        content = file_content;
    } else if (opts->argc >= 1) {
        content = opts->argv[0];
    } else {
        fprintf(stderr, "usage: strata-den ... blob put <content> --tags t1,t2 --roles r1,r2\n");
        return 1;
    }

    char *tags[16], *roles[16];
    int ntags = parse_csv(opts->tags_csv, tags, 16);
    int nroles = parse_csv(opts->roles_csv, roles, 16);
    if (nroles == 0) { free(file_content); free_csv(tags, ntags); fprintf(stderr, "--roles required\n"); return 1; }

    char req[8192];
    int pos = snprintf(req, sizeof(req),
        "{\"action\":\"blob_put\",\"content\":\"%s\",\"entity\":\"%s\",",
        content, opts->entity);
    pos = append_json_array(req, sizeof(req), pos, "tags", tags, ntags);
    pos += snprintf(req + pos, sizeof(req) - pos, ",");
    pos = append_json_array(req, sizeof(req), pos, "roles", roles, nroles);
    snprintf(req + pos, sizeof(req) - pos, "}");

    free(file_content);
    free_csv(tags, ntags);
    free_csv(roles, nroles);

    char resp[8192];
    if (zmq_do_request(sock, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "{\"ok\":false,\"error\":\"timeout\"}\n");
        return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_blob_get(void *sock, den_opts *opts) {
    if (opts->argc < 1) {
        fprintf(stderr, "usage: strata-den ... blob get <blob_id>\n");
        return 1;
    }
    char req[512];
    snprintf(req, sizeof(req),
        "{\"action\":\"blob_get\",\"id\":\"%s\",\"entity\":\"%s\"}",
        opts->argv[0], opts->entity);

    char resp[8192];
    if (zmq_do_request(sock, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "{\"ok\":false,\"error\":\"timeout\"}\n");
        return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_blob_find(void *sock, den_opts *opts) {
    char *tags[16];
    int ntags = parse_csv(opts->tags_csv, tags, 16);

    char req[2048];
    int pos = snprintf(req, sizeof(req),
        "{\"action\":\"blob_find\",\"entity\":\"%s\",", opts->entity);
    pos = append_json_array(req, sizeof(req), pos, "tags", tags, ntags);
    snprintf(req + pos, sizeof(req) - pos, "}");
    free_csv(tags, ntags);

    char resp[16384];
    if (zmq_do_request(sock, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "{\"ok\":false,\"error\":\"timeout\"}\n");
        return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_blob_tag(void *sock, den_opts *opts) {
    if (opts->argc < 2) {
        fprintf(stderr, "usage: strata-den ... blob tag <blob_id> <tag>\n");
        return 1;
    }
    char req[512];
    snprintf(req, sizeof(req),
        "{\"action\":\"blob_tag\",\"id\":\"%s\",\"tag\":\"%s\"}",
        opts->argv[0], opts->argv[1]);

    char resp[4096];
    if (zmq_do_request(sock, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "{\"ok\":false,\"error\":\"timeout\"}\n");
        return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_blob_untag(void *sock, den_opts *opts) {
    if (opts->argc < 2) {
        fprintf(stderr, "usage: strata-den ... blob untag <blob_id> <tag>\n");
        return 1;
    }
    char req[512];
    snprintf(req, sizeof(req),
        "{\"action\":\"blob_untag\",\"id\":\"%s\",\"tag\":\"%s\"}",
        opts->argv[0], opts->argv[1]);

    char resp[4096];
    if (zmq_do_request(sock, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "{\"ok\":false,\"error\":\"timeout\"}\n");
        return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_blob_tags(void *sock, den_opts *opts) {
    if (opts->argc < 1) {
        fprintf(stderr, "usage: strata-den ... blob tags <blob_id>\n");
        return 1;
    }
    char req[512];
    snprintf(req, sizeof(req),
        "{\"action\":\"blob_tags\",\"id\":\"%s\"}", opts->argv[0]);

    char resp[4096];
    if (zmq_do_request(sock, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "{\"ok\":false,\"error\":\"timeout\"}\n");
        return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/*  Listen command (PUB/SUB)                                           */
/* ------------------------------------------------------------------ */

static volatile int listen_running = 1;

static void listen_sigint(int sig) { (void)sig; listen_running = 0; }

static int cmd_listen(den_opts *opts) {
    const char *topic = opts->topic ? opts->topic : "change/";

    signal(SIGINT, listen_sigint);
    signal(SIGTERM, listen_sigint);

    void *ctx = zmq_ctx_new();
    void *sub = zmq_socket(ctx, ZMQ_SUB);
    zmq_connect(sub, opts->endpoint);
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, topic, strlen(topic));

    int timeout = 1000;
    zmq_setsockopt(sub, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));

    while (listen_running) {
        char topic_buf[512] = {0}, payload[8192] = {0};
        int rc = zmq_recv(sub, topic_buf, sizeof(topic_buf) - 1, 0);
        if (rc < 0) continue;
        topic_buf[rc] = '\0';
        rc = zmq_recv(sub, payload, sizeof(payload) - 1, 0);
        if (rc < 0) continue;
        payload[rc] = '\0';

        if (opts->plain)
            printf("[%s] %s\n", topic_buf, payload);
        else
            printf("{\"topic\":\"%s\",\"payload\":%s}\n", topic_buf, payload);
        fflush(stdout);
    }

    zmq_close(sub);
    zmq_ctx_destroy(ctx);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Usage / main                                                       */
/* ------------------------------------------------------------------ */

static void usage(void) {
    fprintf(stderr,
        "usage: strata-den --endpoint <url> --entity <id> [--plain] <command> [args...]\n"
        "\n"
        "Messages:\n"
        "  msg post <repo> <type> <content> --roles r1,r2\n"
        "  msg list <repo> [--type <type>]\n"
        "  msg get <artifact_id>\n"
        "\n"
        "Blobs:\n"
        "  blob put <content> --tags t1,t2 --roles r1,r2\n"
        "  blob put --file <path> --tags t1,t2 --roles r1,r2\n"
        "  blob get <blob_id>\n"
        "  blob find [--tags t1,t2]\n"
        "  blob tag <blob_id> <tag>\n"
        "  blob untag <blob_id> <tag>\n"
        "  blob tags <blob_id>\n"
        "\n"
        "Events:\n"
        "  listen [--topic change/repo-1]\n"
    );
}

int main(int argc, char **argv) {
    den_opts opts = { .timeout_ms = 5000 };

    static struct option long_opts[] = {
        {"endpoint", required_argument, NULL, 'E'},
        {"entity",   required_argument, NULL, 'e'},
        {"plain",    no_argument,       NULL, 'p'},
        {"roles",    required_argument, NULL, 'r'},
        {"tags",     required_argument, NULL, 't'},
        {"type",     required_argument, NULL, 'T'},
        {"file",     required_argument, NULL, 'f'},
        {"topic",    required_argument, NULL, 'o'},
        {"timeout",  required_argument, NULL, 'w'},
        {"help",     no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "E:e:pr:t:T:f:o:w:h", long_opts, NULL)) != -1) {
        switch (c) {
            case 'E': opts.endpoint = optarg; break;
            case 'e': opts.entity = optarg; break;
            case 'p': opts.plain = 1; break;
            case 'r': opts.roles_csv = optarg; break;
            case 't': opts.tags_csv = optarg; break;
            case 'T': opts.type_filter = optarg; break;
            case 'f': opts.file_path = optarg; break;
            case 'o': opts.topic = optarg; break;
            case 'w': opts.timeout_ms = atoi(optarg); break;
            case 'h': usage(); return 0;
            default: usage(); return 1;
        }
    }

    opts.argc = argc - optind;
    opts.argv = argv + optind;

    if (!opts.endpoint) { fprintf(stderr, "--endpoint required\n"); usage(); return 1; }
    if (!opts.entity) { fprintf(stderr, "--entity required\n"); usage(); return 1; }
    if (opts.argc < 1) { usage(); return 1; }

    /* Parse compound command */
    const char *cmd = opts.argv[0];
    const char *subcmd = opts.argc > 1 ? opts.argv[1] : NULL;
    char fullcmd[64];
    int consumed = 1;

    if (subcmd && (strcmp(cmd, "msg") == 0 || strcmp(cmd, "blob") == 0)) {
        snprintf(fullcmd, sizeof(fullcmd), "%s_%s", cmd, subcmd);
        consumed = 2;
    } else {
        strncpy(fullcmd, cmd, sizeof(fullcmd) - 1);
        fullcmd[sizeof(fullcmd) - 1] = '\0';
    }
    opts.argv += consumed;
    opts.argc -= consumed;

    /* Listen is special — uses SUB socket, not REQ */
    if (strcmp(fullcmd, "listen") == 0)
        return cmd_listen(&opts);

    /* All other commands use REQ/REP */
    void *zmq_ctx = zmq_ctx_new();
    void *req = zmq_socket(zmq_ctx, ZMQ_REQ);
    zmq_setsockopt(req, ZMQ_RCVTIMEO, &opts.timeout_ms, sizeof(opts.timeout_ms));
    zmq_connect(req, opts.endpoint);

    int rc = 1;
    if (strcmp(fullcmd, "msg_post") == 0)        rc = cmd_msg_post(req, &opts);
    else if (strcmp(fullcmd, "msg_list") == 0)    rc = cmd_msg_list(req, &opts);
    else if (strcmp(fullcmd, "msg_get") == 0)     rc = cmd_msg_get(req, &opts);
    else if (strcmp(fullcmd, "blob_put") == 0)    rc = cmd_blob_put(req, &opts);
    else if (strcmp(fullcmd, "blob_get") == 0)    rc = cmd_blob_get(req, &opts);
    else if (strcmp(fullcmd, "blob_find") == 0)   rc = cmd_blob_find(req, &opts);
    else if (strcmp(fullcmd, "blob_tag") == 0)    rc = cmd_blob_tag(req, &opts);
    else if (strcmp(fullcmd, "blob_untag") == 0)  rc = cmd_blob_untag(req, &opts);
    else if (strcmp(fullcmd, "blob_tags") == 0)   rc = cmd_blob_tags(req, &opts);
    else { fprintf(stderr, "unknown command: %s\n", fullcmd); usage(); rc = 1; }

    zmq_close(req);
    zmq_ctx_destroy(zmq_ctx);
    return rc;
}
