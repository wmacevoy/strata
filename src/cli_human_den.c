/*
 * Human den — interactive REPL that runs as a proper villager.
 *
 * Unlike the strata CLI (fire-and-forget commands), the human den is a
 * persistent process in the village. It has:
 *   - ZMQ REQ socket to store_service (same as every den)
 *   - ZMQ SUB socket for events (sees what happens in the village)
 *   - Interactive command prompt for the human
 *
 * This is how a person becomes a villager — same door as every den.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>
#include <zmq.h>
#include "strata/aead.h"
#include "strata/json_util.h"

static volatile int running = 1;

static void sigint_handler(int sig) { (void)sig; running = 0; }

/* ------------------------------------------------------------------ */
/*  Agent registry (for talk command)                                   */
/* ------------------------------------------------------------------ */

#define MAX_AGENTS 16

typedef struct {
    char name[64];
    char endpoint[256];
} agent_entry;

static agent_entry agents[MAX_AGENTS];
static int num_agents = 0;

static void agent_register(const char *name, const char *endpoint) {
    if (num_agents >= MAX_AGENTS) return;
    strncpy(agents[num_agents].name, name, sizeof(agents[0].name) - 1);
    strncpy(agents[num_agents].endpoint, endpoint, sizeof(agents[0].endpoint) - 1);
    num_agents++;
}

static const char *agent_lookup(const char *name) {
    for (int i = 0; i < num_agents; i++) {
        if (strcmp(agents[i].name, name) == 0)
            return agents[i].endpoint;
    }
    return NULL;
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

/* ------------------------------------------------------------------ */
/*  Event listener thread                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *sub_endpoint;
    const char *topic;
} listener_args;

static void *event_listener(void *arg) {
    listener_args *la = arg;

    void *ctx = zmq_ctx_new();
    void *sub = zmq_socket(ctx, ZMQ_SUB);
    zmq_connect(sub, la->sub_endpoint);
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, la->topic, strlen(la->topic));

    int timeout = 500;
    zmq_setsockopt(sub, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));

    while (running) {
        char topic_buf[512] = {0}, payload[8192] = {0};
        int rc = zmq_recv(sub, topic_buf, sizeof(topic_buf) - 1, 0);
        if (rc < 0) continue;
        topic_buf[rc] = '\0';
        rc = zmq_recv(sub, payload, sizeof(payload) - 1, 0);
        if (rc < 0) continue;
        payload[rc] = '\0';

        /* Print event, then reprint the prompt */
        printf("\n  [event] %s: %s\n> ", topic_buf, payload);
        fflush(stdout);
    }

    zmq_close(sub);
    zmq_ctx_destroy(ctx);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Command handlers                                                   */
/* ------------------------------------------------------------------ */

static void cmd_help(void) {
    printf(
        "Commands:\n"
        "  talk <agent> <message>       — talk to an agent directly\n"
        "  msg post <repo> <type> <content> <roles_csv>\n"
        "  msg list <repo> [type]\n"
        "  msg get <artifact_id>\n"
        "  blob put <content> <tags_csv> <roles_csv>\n"
        "  blob get <blob_id>\n"
        "  blob find [tags_csv]\n"
        "  blob tag <blob_id> <tag>\n"
        "  blob untag <blob_id> <tag>\n"
        "  blob tags <blob_id>\n"
        "  repo create <repo_id> <name>\n"
        "  role assign <entity> <role> <repo>\n"
        "  role revoke <entity> <role> <repo>\n"
        "  entity register <entity_id>\n"
        "  privilege grant <entity> <privilege>\n"
        "  privilege revoke <entity> <privilege>\n"
        "  privilege check <entity> <privilege>\n"
        "  whoami\n"
        "  help\n"
        "  quit\n"
    );
}

/* Build a JSON array from CSV string */
static int build_json_array(char *buf, int cap, int pos,
                             const char *key, const char *csv) {
    pos += snprintf(buf + pos, cap - pos, "\"%s\":[", key);
    if (csv && csv[0]) {
        char *copy = strdup(csv);
        char *tok = strtok(copy, ",");
        int first = 1;
        while (tok) {
            pos += snprintf(buf + pos, cap - pos, "%s\"%s\"", first ? "" : ",", tok);
            first = 0;
            tok = strtok(NULL, ",");
        }
        free(copy);
    }
    pos += snprintf(buf + pos, cap - pos, "]");
    return pos;
}

static void handle_line(void *zmq_ctx, void *req_sock, const char *entity, const char *line) {
    /* Tokenize the line */
    char buf[4096];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *all_tokens[16] = {0};
    int ntok_total = 0;
    char *tok = strtok(buf, " \t");
    while (tok && ntok_total < 16) {
        all_tokens[ntok_total++] = tok;
        tok = strtok(NULL, " \t");
    }
    if (ntok_total == 0) return;

    char req[8192];
    char resp[16384];

    /* Compound commands */
    char cmd[64] = {0};
    char **tokens;
    int ntok;
    if (ntok_total >= 2 && (strcmp(all_tokens[0], "msg") == 0 || strcmp(all_tokens[0], "blob") == 0 ||
                      strcmp(all_tokens[0], "repo") == 0 || strcmp(all_tokens[0], "role") == 0 ||
                      strcmp(all_tokens[0], "entity") == 0 || strcmp(all_tokens[0], "privilege") == 0)) {
        snprintf(cmd, sizeof(cmd), "%s_%s", all_tokens[0], all_tokens[1]);
        tokens = all_tokens + 2; ntok = ntok_total - 2;
    } else {
        strncpy(cmd, all_tokens[0], sizeof(cmd) - 1);
        tokens = all_tokens + 1; ntok = ntok_total - 1;
    }

    if (strcmp(cmd, "help") == 0) {
        cmd_help();
        return;
    }
    if (strcmp(cmd, "whoami") == 0) {
        printf("entity: %s\n", entity);
        return;
    }
    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        running = 0;
        return;
    }

    /* talk <agent> <message...> */
    if (strcmp(cmd, "talk") == 0) {
        if (ntok < 2) { printf("usage: talk <agent> <message>\n"); return; }
        const char *agent_name = tokens[0];
        const char *ep = agent_lookup(agent_name);
        if (!ep) {
            /* Try treating it as a raw endpoint */
            if (strncmp(agent_name, "tcp://", 6) == 0 || strncmp(agent_name, "ipc://", 6) == 0)
                ep = agent_name;
            else {
                printf("unknown agent: %s (register with --agent %s=<endpoint>)\n", agent_name, agent_name);
                return;
            }
        }
        /* Reassemble message from remaining tokens */
        char message[4096] = {0};
        int mpos = 0;
        for (int i = 1; i < ntok && mpos < (int)sizeof(message) - 1; i++) {
            if (i > 1) message[mpos++] = ' ';
            mpos += snprintf(message + mpos, sizeof(message) - mpos, "%s", tokens[i]);
        }
        /* Create temporary REQ socket to agent */
        void *agent_sock = zmq_socket(zmq_ctx, ZMQ_REQ);
        int talk_timeout = 5000;
        zmq_setsockopt(agent_sock, ZMQ_RCVTIMEO, &talk_timeout, sizeof(talk_timeout));
        zmq_connect(agent_sock, ep);

        char talk_req[8192];
        char esc_message[4096];
        json_escape(message, (int)strlen(message), esc_message, sizeof(esc_message));
        snprintf(talk_req, sizeof(talk_req),
            "{\"action\":\"say\",\"from\":\"%s\",\"message\":\"%s\"}",
            entity, esc_message);

        char talk_resp[16384];
        if (zmq_do_request(agent_sock, talk_req, talk_resp, sizeof(talk_resp)) < 0) {
            printf("error: %s did not respond\n", agent_name);
        } else {
            printf("%s: %s\n", agent_name, talk_resp);
        }
        zmq_close(agent_sock);
        return;
    }

    /* msg post <repo> <type> <content> <roles_csv> */
    if (strcmp(cmd, "msg_post") == 0) {
        if (ntok < 4) { printf("usage: msg post <repo> <type> <content> <roles_csv>\n"); return; }
        int pos = snprintf(req, sizeof(req),
            "{\"action\":\"put\",\"repo\":\"%s\",\"type\":\"%s\","
            "\"content\":\"%s\",\"author\":\"%s\",",
            tokens[0], tokens[1], tokens[2], entity);
        pos = build_json_array(req, sizeof(req), pos, "roles", tokens[3]);
        snprintf(req + pos, sizeof(req) - pos, "}");
    }
    /* msg list <repo> [type] */
    else if (strcmp(cmd, "msg_list") == 0) {
        if (ntok < 1) { printf("usage: msg list <repo> [type]\n"); return; }
        if (ntok >= 2)
            snprintf(req, sizeof(req),
                "{\"action\":\"list\",\"repo\":\"%s\",\"type\":\"%s\",\"entity\":\"%s\"}",
                tokens[0], tokens[1], entity);
        else
            snprintf(req, sizeof(req),
                "{\"action\":\"list\",\"repo\":\"%s\",\"entity\":\"%s\"}",
                tokens[0], entity);
    }
    /* msg get <id> */
    else if (strcmp(cmd, "msg_get") == 0) {
        if (ntok < 1) { printf("usage: msg get <artifact_id>\n"); return; }
        snprintf(req, sizeof(req),
            "{\"action\":\"get\",\"id\":\"%s\",\"entity\":\"%s\"}",
            tokens[0], entity);
    }
    /* blob put <content> <tags_csv> <roles_csv> */
    else if (strcmp(cmd, "blob_put") == 0) {
        if (ntok < 3) { printf("usage: blob put <content> <tags_csv> <roles_csv>\n"); return; }
        int pos = snprintf(req, sizeof(req),
            "{\"action\":\"blob_put\",\"content\":\"%s\",\"entity\":\"%s\",",
            tokens[0], entity);
        pos = build_json_array(req, sizeof(req), pos, "tags", tokens[1]);
        pos += snprintf(req + pos, sizeof(req) - pos, ",");
        pos = build_json_array(req, sizeof(req), pos, "roles", tokens[2]);
        snprintf(req + pos, sizeof(req) - pos, "}");
    }
    /* blob get <id> */
    else if (strcmp(cmd, "blob_get") == 0) {
        if (ntok < 1) { printf("usage: blob get <blob_id>\n"); return; }
        snprintf(req, sizeof(req),
            "{\"action\":\"blob_get\",\"id\":\"%s\",\"entity\":\"%s\"}",
            tokens[0], entity);
    }
    /* blob find [tags_csv] */
    else if (strcmp(cmd, "blob_find") == 0) {
        int pos = snprintf(req, sizeof(req),
            "{\"action\":\"blob_find\",\"entity\":\"%s\",", entity);
        pos = build_json_array(req, sizeof(req), pos, "tags", ntok > 0 ? tokens[0] : "");
        snprintf(req + pos, sizeof(req) - pos, "}");
    }
    /* blob tag <id> <tag> */
    else if (strcmp(cmd, "blob_tag") == 0) {
        if (ntok < 2) { printf("usage: blob tag <blob_id> <tag>\n"); return; }
        snprintf(req, sizeof(req),
            "{\"action\":\"blob_tag\",\"id\":\"%s\",\"tag\":\"%s\"}",
            tokens[0], tokens[1]);
    }
    /* blob untag <id> <tag> */
    else if (strcmp(cmd, "blob_untag") == 0) {
        if (ntok < 2) { printf("usage: blob untag <blob_id> <tag>\n"); return; }
        snprintf(req, sizeof(req),
            "{\"action\":\"blob_untag\",\"id\":\"%s\",\"tag\":\"%s\"}",
            tokens[0], tokens[1]);
    }
    /* blob tags <id> */
    else if (strcmp(cmd, "blob_tags") == 0) {
        if (ntok < 1) { printf("usage: blob tags <blob_id>\n"); return; }
        snprintf(req, sizeof(req),
            "{\"action\":\"blob_tags\",\"id\":\"%s\"}", tokens[0]);
    }
    /* repo create <id> <name> */
    else if (strcmp(cmd, "repo_create") == 0) {
        if (ntok < 2) { printf("usage: repo create <repo_id> <name>\n"); return; }
        snprintf(req, sizeof(req),
            "{\"action\":\"repo_create\",\"repo\":\"%s\",\"name\":\"%s\"}",
            tokens[0], tokens[1]);
    }
    /* role assign <entity> <role> <repo> */
    else if (strcmp(cmd, "role_assign") == 0) {
        if (ntok < 3) { printf("usage: role assign <entity> <role> <repo>\n"); return; }
        snprintf(req, sizeof(req),
            "{\"action\":\"role_assign\",\"entity\":\"%s\",\"role\":\"%s\",\"repo\":\"%s\"}",
            tokens[0], tokens[1], tokens[2]);
    }
    /* role revoke <entity> <role> <repo> */
    else if (strcmp(cmd, "role_revoke") == 0) {
        if (ntok < 3) { printf("usage: role revoke <entity> <role> <repo>\n"); return; }
        snprintf(req, sizeof(req),
            "{\"action\":\"role_revoke\",\"entity\":\"%s\",\"role\":\"%s\",\"repo\":\"%s\"}",
            tokens[0], tokens[1], tokens[2]);
    }
    /* entity register <entity_id> */
    else if (strcmp(cmd, "entity_register") == 0) {
        if (ntok < 1) { printf("usage: entity register <entity_id>\n"); return; }
        snprintf(req, sizeof(req),
            "{\"action\":\"entity_register\",\"entity\":\"%s\"}", tokens[0]);
    }
    /* privilege grant/revoke/check */
    else if (strcmp(cmd, "privilege_grant") == 0) {
        if (ntok < 2) { printf("usage: privilege grant <entity> <privilege>\n"); return; }
        snprintf(req, sizeof(req),
            "{\"action\":\"privilege_grant\",\"entity\":\"%s\",\"privilege\":\"%s\"}",
            tokens[0], tokens[1]);
    }
    else if (strcmp(cmd, "privilege_revoke") == 0) {
        if (ntok < 2) { printf("usage: privilege revoke <entity> <privilege>\n"); return; }
        snprintf(req, sizeof(req),
            "{\"action\":\"privilege_revoke\",\"entity\":\"%s\",\"privilege\":\"%s\"}",
            tokens[0], tokens[1]);
    }
    else if (strcmp(cmd, "privilege_check") == 0) {
        if (ntok < 2) { printf("usage: privilege check <entity> <privilege>\n"); return; }
        snprintf(req, sizeof(req),
            "{\"action\":\"privilege_check\",\"entity\":\"%s\",\"privilege\":\"%s\"}",
            tokens[0], tokens[1]);
    }
    else {
        printf("unknown command: %s (type 'help' for commands)\n", cmd);
        return;
    }

    if (zmq_do_request(req_sock, req, resp, sizeof(resp)) < 0) {
        printf("error: timeout\n");
    } else {
        printf("%s\n", resp);
    }
}

/* ------------------------------------------------------------------ */
/*  Usage / main                                                       */
/* ------------------------------------------------------------------ */

static void usage(void) {
    fprintf(stderr,
        "usage: strata-human --endpoint <store_url> --entity <id> [--sub <pub_url>] [--topic <filter>]\n"
        "                    [--agent name=endpoint ...]\n"
        "\n"
        "  --endpoint  Store service ZMQ endpoint (REQ/REP)\n"
        "  --entity    Your identity in the village\n"
        "  --sub       PUB endpoint to subscribe for events (optional)\n"
        "  --topic     Event topic filter (default: change/)\n"
        "  --agent     Register agent for talk command (e.g. --agent gee=tcp://127.0.0.1:5570)\n"
    );
}

int main(int argc, char **argv) {
    const char *endpoint = NULL;
    const char *entity = NULL;
    const char *sub_endpoint = NULL;
    const char *topic = "change/";

    static struct option long_opts[] = {
        {"endpoint", required_argument, NULL, 'E'},
        {"entity",   required_argument, NULL, 'e'},
        {"sub",      required_argument, NULL, 's'},
        {"topic",    required_argument, NULL, 'o'},
        {"agent",    required_argument, NULL, 'a'},
        {"help",     no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "E:e:s:o:a:h", long_opts, NULL)) != -1) {
        switch (c) {
            case 'E': endpoint = optarg; break;
            case 'e': entity = optarg; break;
            case 's': sub_endpoint = optarg; break;
            case 'o': topic = optarg; break;
            case 'a': {
                /* Parse name=endpoint */
                char *eq = strchr(optarg, '=');
                if (!eq) { fprintf(stderr, "--agent format: name=endpoint\n"); return 1; }
                *eq = '\0';
                agent_register(optarg, eq + 1);
                break;
            }
            case 'h': usage(); return 0;
            default: usage(); return 1;
        }
    }

    if (!endpoint) { fprintf(stderr, "--endpoint required\n"); usage(); return 1; }
    if (!entity) { fprintf(stderr, "--entity required\n"); usage(); return 1; }

    signal(SIGINT, sigint_handler);

    /* Connect REQ socket to store service */
    void *zmq_ctx = zmq_ctx_new();
    void *req = zmq_socket(zmq_ctx, ZMQ_REQ);
    int timeout = 5000;
    zmq_setsockopt(req, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_connect(req, endpoint);

    /* Start event listener thread if SUB endpoint provided */
    pthread_t listener_thread;
    listener_args la = { sub_endpoint, topic };
    if (sub_endpoint) {
        pthread_create(&listener_thread, NULL, event_listener, &la);
    }

    printf("strata human den: %s\n", entity);
    printf("store: %s\n", endpoint);
    if (sub_endpoint)
        printf("events: %s (topic: %s)\n", sub_endpoint, topic);
    for (int i = 0; i < num_agents; i++)
        printf("agent: %s @ %s\n", agents[i].name, agents[i].endpoint);
    printf("type 'help' for commands, 'quit' to exit\n\n");

    /* REPL */
    char line[4096];
    while (running) {
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        /* Strip newline */
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (len == 0) continue;

        handle_line(zmq_ctx, req, entity, line);
    }

    printf("\nleaving village\n");

    /* Cleanup */
    running = 0;
    if (sub_endpoint) {
        pthread_join(listener_thread, NULL);
    }
    zmq_close(req);
    zmq_ctx_destroy(zmq_ctx);
    return 0;
}
