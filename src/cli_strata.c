/*
 * strata CLI — village builder.
 *
 * Two modes:
 *   --db <path>       Bootstrap mode (init only). Direct SQLite for schema creation.
 *   --endpoint <url>  Villager mode. All commands via TCP REQ/REP to store_service.
 *
 * All commands except 'init' require --endpoint. The CLI talks to the store
 * service over TCP like every other villager. No direct database access.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include "strata/msg.h"
#include "strata/aead.h"
#include "strata/store.h"
#include "strata/json_util.h"

/* ------------------------------------------------------------------ */
/*  Options                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *db_path;
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
} cli_opts;

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
/*  Transport helpers                                                  */
/* ------------------------------------------------------------------ */

static int do_request(const char *endpoint, const char *request,
                      char *resp, int resp_cap) {
    strata_sock *sock = strata_req_connect(endpoint);
    if (!sock) return -1;
    strata_msg_set_timeout(sock, 5000, 5000);
    int rc = strata_send(sock, request, strlen(request), 0);
    if (rc < 0) { strata_sock_close(sock); return -1; }
    rc = strata_recv(sock, resp, resp_cap - 1, 0);
    strata_sock_close(sock);
    if (rc >= 0) resp[rc] = '\0';
    return rc;
}

/* Build a JSON array fragment: "key":["a","b"] */
static int append_json_array(char *buf, int cap, int pos,
                              const char *key, char **items, int count) {
    pos += snprintf(buf + pos, cap - pos, "\"%s\":[", key);
    for (int i = 0; i < count; i++)
        pos += snprintf(buf + pos, cap - pos, "%s\"%s\"", i > 0 ? "," : "", items[i]);
    pos += snprintf(buf + pos, cap - pos, "]");
    return pos;
}

/* ------------------------------------------------------------------ */
/*  Bootstrap: init (direct SQLite — only for first-time setup)        */
/* ------------------------------------------------------------------ */

static int cmd_init(cli_opts *opts) {
    if (!opts->db_path) {
        fprintf(stderr, "--db required for init\n");
        return 1;
    }
    strata_store *store = strata_store_open_sqlite(opts->db_path);
    if (!store) { fprintf(stderr, "failed to open database: %s\n", opts->db_path); return 1; }
    strata_store_init(store);
    strata_store_close(store);
    if (opts->plain)
        printf("initialized\n");
    else
        printf("{\"ok\":true}\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Commands: everything goes through the store service                */
/* ------------------------------------------------------------------ */

static int cmd_repo_create(const char *endpoint, cli_opts *opts) {
    if (opts->argc < 2) {
        fprintf(stderr, "usage: strata --endpoint <url> repo create <repo_id> <name>\n");
        return 1;
    }
    char req[2048];
    snprintf(req, sizeof(req),
        "{\"action\":\"repo_create\",\"repo\":\"%s\",\"name\":\"%s\"}",
        opts->argv[0], opts->argv[1]);

    char resp[4096];
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_role_assign(const char *endpoint, cli_opts *opts) {
    if (opts->argc < 3) {
        fprintf(stderr, "usage: strata --endpoint <url> role assign <entity> <role> <repo_id>\n");
        return 1;
    }
    char req[2048];
    snprintf(req, sizeof(req),
        "{\"action\":\"role_assign\",\"entity\":\"%s\",\"role\":\"%s\",\"repo\":\"%s\"}",
        opts->argv[0], opts->argv[1], opts->argv[2]);

    char resp[4096];
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_role_revoke(const char *endpoint, cli_opts *opts) {
    if (opts->argc < 3) {
        fprintf(stderr, "usage: strata --endpoint <url> role revoke <entity> <role> <repo_id>\n");
        return 1;
    }
    char req[2048];
    snprintf(req, sizeof(req),
        "{\"action\":\"role_revoke\",\"entity\":\"%s\",\"role\":\"%s\",\"repo\":\"%s\"}",
        opts->argv[0], opts->argv[1], opts->argv[2]);

    char resp[4096];
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_msg_post(const char *endpoint, cli_opts *opts) {
    if (!opts->entity) { fprintf(stderr, "--entity required\n"); return 1; }
    if (opts->argc < 3) {
        fprintf(stderr, "usage: strata --endpoint <url> --entity <id> msg post <repo> <type> <content> --roles r1,r2\n");
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
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_msg_list(const char *endpoint, cli_opts *opts) {
    if (!opts->entity) { fprintf(stderr, "--entity required\n"); return 1; }
    if (opts->argc < 1) {
        fprintf(stderr, "usage: strata --endpoint <url> --entity <id> msg list <repo> [--type <type>]\n");
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
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_msg_get(const char *endpoint, cli_opts *opts) {
    if (!opts->entity) { fprintf(stderr, "--entity required\n"); return 1; }
    if (opts->argc < 1) {
        fprintf(stderr, "usage: strata --endpoint <url> --entity <id> msg get <artifact_id>\n");
        return 1;
    }
    char req[512];
    snprintf(req, sizeof(req),
        "{\"action\":\"get\",\"id\":\"%s\",\"entity\":\"%s\"}",
        opts->argv[0], opts->entity);

    char resp[8192];
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_blob_put(const char *endpoint, cli_opts *opts) {
    if (!opts->entity) { fprintf(stderr, "--entity required\n"); return 1; }

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
        fprintf(stderr, "usage: strata --endpoint <url> --entity <id> blob put <content> --tags t1,t2 --roles r1,r2\n");
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
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_blob_get(const char *endpoint, cli_opts *opts) {
    if (!opts->entity) { fprintf(stderr, "--entity required\n"); return 1; }
    if (opts->argc < 1) {
        fprintf(stderr, "usage: strata --endpoint <url> --entity <id> blob get <blob_id>\n");
        return 1;
    }
    char req[512];
    snprintf(req, sizeof(req),
        "{\"action\":\"blob_get\",\"id\":\"%s\",\"entity\":\"%s\"}",
        opts->argv[0], opts->entity);

    char resp[8192];
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_blob_find(const char *endpoint, cli_opts *opts) {
    if (!opts->entity) { fprintf(stderr, "--entity required\n"); return 1; }

    char *tags[16];
    int ntags = parse_csv(opts->tags_csv, tags, 16);

    char req[2048];
    int pos = snprintf(req, sizeof(req),
        "{\"action\":\"blob_find\",\"entity\":\"%s\",", opts->entity);
    pos = append_json_array(req, sizeof(req), pos, "tags", tags, ntags);
    snprintf(req + pos, sizeof(req) - pos, "}");
    free_csv(tags, ntags);

    char resp[16384];
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_blob_tag(const char *endpoint, cli_opts *opts) {
    if (opts->argc < 2) {
        fprintf(stderr, "usage: strata --endpoint <url> blob tag <blob_id> <tag>\n");
        return 1;
    }
    char req[512];
    snprintf(req, sizeof(req),
        "{\"action\":\"blob_tag\",\"id\":\"%s\",\"tag\":\"%s\"}",
        opts->argv[0], opts->argv[1]);

    char resp[4096];
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_blob_untag(const char *endpoint, cli_opts *opts) {
    if (opts->argc < 2) {
        fprintf(stderr, "usage: strata --endpoint <url> blob untag <blob_id> <tag>\n");
        return 1;
    }
    char req[512];
    snprintf(req, sizeof(req),
        "{\"action\":\"blob_untag\",\"id\":\"%s\",\"tag\":\"%s\"}",
        opts->argv[0], opts->argv[1]);

    char resp[4096];
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_blob_tags(const char *endpoint, cli_opts *opts) {
    if (opts->argc < 1) {
        fprintf(stderr, "usage: strata --endpoint <url> blob tags <blob_id>\n");
        return 1;
    }
    char req[512];
    snprintf(req, sizeof(req),
        "{\"action\":\"blob_tags\",\"id\":\"%s\"}", opts->argv[0]);

    char resp[4096];
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_privilege_grant(const char *endpoint, cli_opts *opts) {
    if (opts->argc < 2) {
        fprintf(stderr, "usage: strata --endpoint <url> privilege grant <entity> <privilege>\n");
        return 1;
    }
    char req[512];
    snprintf(req, sizeof(req),
        "{\"action\":\"privilege_grant\",\"entity\":\"%s\",\"privilege\":\"%s\"}",
        opts->argv[0], opts->argv[1]);

    char resp[4096];
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_privilege_revoke(const char *endpoint, cli_opts *opts) {
    if (opts->argc < 2) {
        fprintf(stderr, "usage: strata --endpoint <url> privilege revoke <entity> <privilege>\n");
        return 1;
    }
    char req[512];
    snprintf(req, sizeof(req),
        "{\"action\":\"privilege_revoke\",\"entity\":\"%s\",\"privilege\":\"%s\"}",
        opts->argv[0], opts->argv[1]);

    char resp[4096];
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_privilege_check(const char *endpoint, cli_opts *opts) {
    if (opts->argc < 2) {
        fprintf(stderr, "usage: strata --endpoint <url> privilege check <entity> <privilege>\n");
        return 1;
    }
    char req[512];
    snprintf(req, sizeof(req),
        "{\"action\":\"privilege_check\",\"entity\":\"%s\",\"privilege\":\"%s\"}",
        opts->argv[0], opts->argv[1]);

    char resp[4096];
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/*  Entity commands                                                    */
/* ------------------------------------------------------------------ */

static int cmd_entity_register(const char *endpoint, cli_opts *opts) {
    if (opts->argc < 1) {
        fprintf(stderr, "usage: strata --endpoint <url> entity register <entity_id>\n");
        return 1;
    }
    char req[512];
    snprintf(req, sizeof(req),
        "{\"action\":\"entity_register\",\"entity\":\"%s\"}", opts->argv[0]);

    char resp[4096];
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

static int cmd_entity_auth(const char *endpoint, cli_opts *opts) {
    if (opts->argc < 2) {
        fprintf(stderr, "usage: strata --endpoint <url> entity auth <entity_id> <token>\n");
        return 1;
    }
    char req[512];
    snprintf(req, sizeof(req),
        "{\"action\":\"entity_authenticate\",\"entity\":\"%s\",\"token\":\"%s\"}",
        opts->argv[0], opts->argv[1]);

    char resp[4096];
    if (do_request(endpoint, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "timeout\n"); return 1;
    }
    printf("%s\n", resp);
    return strstr(resp, "\"ok\":true") ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/*  Listen command (PUB/SUB)                                           */
/* ------------------------------------------------------------------ */

static volatile int listen_running = 1;

static void listen_sigint(int sig) { (void)sig; listen_running = 0; }

static int cmd_listen(cli_opts *opts) {
    const char *topic = opts->topic ? opts->topic : "change/";

    signal(SIGINT, listen_sigint);
    signal(SIGTERM, listen_sigint);

    strata_sock *sub = strata_sub_connect(opts->endpoint);
    if (!sub) {
        fprintf(stderr, "failed to connect to %s\n", opts->endpoint);
        return 1;
    }
    strata_sub_subscribe(sub, topic);
    strata_msg_set_timeout(sub, 1000, -1);

    while (listen_running) {
        char topic_buf[512], payload[8192];
        int rc = strata_sub_recv(sub, topic_buf, sizeof(topic_buf),
                                 payload, sizeof(payload));
        if (rc < 0) continue;

        if (opts->plain)
            printf("[%s] %s\n", topic_buf, payload);
        else
            printf("{\"topic\":\"%s\",\"payload\":%s}\n", topic_buf, payload);
        fflush(stdout);
    }

    strata_sock_close(sub);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Usage / main                                                       */
/* ------------------------------------------------------------------ */

static void usage(void) {
    fprintf(stderr,
        "usage: strata [--db <path>] [--endpoint <url>] [--entity <id>] [--plain] <command> [args...]\n"
        "\n"
        "Bootstrap (requires --db):\n"
        "  init                              Initialize schema\n"
        "\n"
        "Admin (requires --endpoint):\n"
        "  repo create <id> <name>           Create repository\n"
        "  role assign <entity> <role> <repo> Assign role\n"
        "  role revoke <entity> <role> <repo> Revoke role\n"
        "\n"
        "Privileges (requires --endpoint):\n"
        "  privilege grant <entity> <priv>   Grant privilege (core/parent/vocation)\n"
        "  privilege revoke <entity> <priv>  Revoke privilege\n"
        "  privilege check <entity> <priv>   Check if entity has privilege\n"
        "\n"
        "Messages (requires --endpoint --entity):\n"
        "  msg post <repo> <type> <content> --roles r1,r2\n"
        "  msg list <repo> [--type <type>]\n"
        "  msg get <artifact_id>\n"
        "\n"
        "Blobs (requires --endpoint --entity for put/get/find):\n"
        "  blob put <content> --tags t1,t2 --roles r1,r2\n"
        "  blob put --file <path> --tags t1,t2 --roles r1,r2\n"
        "  blob get <blob_id>\n"
        "  blob find [--tags t1,t2]\n"
        "  blob tag <blob_id> <tag>\n"
        "  blob untag <blob_id> <tag>\n"
        "  blob tags <blob_id>\n"
        "\n"
        "Identity (requires --endpoint):\n"
        "  entity register <entity_id>       Register entity, get token\n"
        "  entity auth <entity_id> <token>   Verify token\n"
        "\n"
        "Events (requires --endpoint for PUB/SUB endpoint):\n"
        "  listen [--topic change/repo-1]\n"
    );
}

int main(int argc, char **argv) {
    cli_opts opts = { .timeout_ms = 5000 };

    static struct option long_opts[] = {
        {"db",       required_argument, NULL, 'd'},
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
    while ((c = getopt_long(argc, argv, "d:E:e:pr:t:T:f:o:w:h", long_opts, NULL)) != -1) {
        switch (c) {
            case 'd': opts.db_path = optarg; break;
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

    if (!opts.db_path && !opts.endpoint) {
        fprintf(stderr, "--db or --endpoint required\n");
        usage();
        return 1;
    }
    if (opts.argc < 1) { usage(); return 1; }

    /* Parse compound command */
    const char *cmd = opts.argv[0];
    const char *subcmd = opts.argc > 1 ? opts.argv[1] : NULL;
    char fullcmd[64];
    int consumed = 1;

    if (subcmd && (strcmp(cmd, "repo") == 0 || strcmp(cmd, "role") == 0 ||
                   strcmp(cmd, "msg") == 0 || strcmp(cmd, "blob") == 0 ||
                   strcmp(cmd, "privilege") == 0 || strcmp(cmd, "entity") == 0)) {
        snprintf(fullcmd, sizeof(fullcmd), "%s_%s", cmd, subcmd);
        consumed = 2;
    } else {
        strncpy(fullcmd, cmd, sizeof(fullcmd) - 1);
        fullcmd[sizeof(fullcmd) - 1] = '\0';
    }
    opts.argv += consumed;
    opts.argc -= consumed;

    /* Bootstrap command: init (direct SQLite) */
    if (strcmp(fullcmd, "init") == 0)
        return cmd_init(&opts);

    /* Listen is special — uses SUB socket, not REQ */
    if (strcmp(fullcmd, "listen") == 0) {
        if (!opts.endpoint) { fprintf(stderr, "--endpoint required\n"); return 1; }
        return cmd_listen(&opts);
    }

    /* All other commands: per-request TCP connections to store service */
    if (!opts.endpoint) {
        fprintf(stderr, "--endpoint required (use --db only for init)\n");
        return 1;
    }

    int rc = 1;
    if (strcmp(fullcmd, "repo_create") == 0)        rc = cmd_repo_create(opts.endpoint, &opts);
    else if (strcmp(fullcmd, "role_assign") == 0)    rc = cmd_role_assign(opts.endpoint, &opts);
    else if (strcmp(fullcmd, "role_revoke") == 0)    rc = cmd_role_revoke(opts.endpoint, &opts);
    else if (strcmp(fullcmd, "msg_post") == 0)       rc = cmd_msg_post(opts.endpoint, &opts);
    else if (strcmp(fullcmd, "msg_list") == 0)       rc = cmd_msg_list(opts.endpoint, &opts);
    else if (strcmp(fullcmd, "msg_get") == 0)        rc = cmd_msg_get(opts.endpoint, &opts);
    else if (strcmp(fullcmd, "blob_put") == 0)       rc = cmd_blob_put(opts.endpoint, &opts);
    else if (strcmp(fullcmd, "blob_get") == 0)       rc = cmd_blob_get(opts.endpoint, &opts);
    else if (strcmp(fullcmd, "blob_find") == 0)      rc = cmd_blob_find(opts.endpoint, &opts);
    else if (strcmp(fullcmd, "blob_tag") == 0)       rc = cmd_blob_tag(opts.endpoint, &opts);
    else if (strcmp(fullcmd, "blob_untag") == 0)     rc = cmd_blob_untag(opts.endpoint, &opts);
    else if (strcmp(fullcmd, "blob_tags") == 0)      rc = cmd_blob_tags(opts.endpoint, &opts);
    else if (strcmp(fullcmd, "privilege_grant") == 0)  rc = cmd_privilege_grant(opts.endpoint, &opts);
    else if (strcmp(fullcmd, "privilege_revoke") == 0) rc = cmd_privilege_revoke(opts.endpoint, &opts);
    else if (strcmp(fullcmd, "privilege_check") == 0)  rc = cmd_privilege_check(opts.endpoint, &opts);
    else if (strcmp(fullcmd, "entity_register") == 0)  rc = cmd_entity_register(opts.endpoint, &opts);
    else if (strcmp(fullcmd, "entity_auth") == 0)      rc = cmd_entity_auth(opts.endpoint, &opts);
    else { fprintf(stderr, "unknown command: %s\n", fullcmd); usage(); rc = 1; }

    return rc;
}
