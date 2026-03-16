/*
 * cisco — a tiny village.
 *
 * store + anthropic (privileged JS den) + claudette (sandboxed JS den).
 * No messenger needed — anthropic uses bedrock.fetch directly.
 * A human walks in with strata-human.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include "strata/msg.h"
#include "strata/aead.h"
#include "strata/store.h"
#include "strata/den.h"
#include "strata/json_util.h"

extern int store_service_run(const char *db_path, const char *endpoint);

#define DB_PATH         "/tmp/cisco.db"
#define PID_FILE        "/tmp/cisco.pid"
#define STORE_EP        "tcp://127.0.0.1:6560"
#define ANTHROPIC_REP   "tcp://127.0.0.1:6570"
#define ANTHROPIC_PUB   "tcp://127.0.0.1:6580"
#define CLAUDETTE_REP   "tcp://127.0.0.1:6571"
#define CLAUDETTE_PUB   "tcp://127.0.0.1:6581"
#define LIBRARY_REP     "tcp://127.0.0.1:6572"
#define LIBRARY_PUB     "tcp://127.0.0.1:6582"

static volatile int running = 1;
static pid_t store_pid = -1;
static pid_t anthropic_pid = -1;
static pid_t claudette_pid = -1;
static pid_t library_pid = -1;
static strata_den_host *host = NULL;

static void kill_child(pid_t *pid) {
    if (*pid > 0) { kill(*pid, SIGTERM); waitpid(*pid, NULL, 0); *pid = -1; }
}

static void cleanup(void) {
    kill_child(&claudette_pid);
    kill_child(&library_pid);
    kill_child(&anthropic_pid);
    if (host) { strata_den_host_free(host); host = NULL; }
    kill_child(&store_pid);
    unlink(PID_FILE);
}

static void signal_handler(int sig) { (void)sig; running = 0; }
static void abort_handler(int sig) { (void)sig; cleanup(); _exit(1); }

static int wait_for_service(const char *endpoint, int max_retries) {
    for (int i = 0; i < max_retries; i++) {
        usleep(100000);
        strata_sock *sock = strata_req_connect(endpoint);
        if (!sock) continue;
        strata_msg_set_timeout(sock, 500, 500);
        const char *probe = "{\"action\":\"init\"}";
        if (strata_msg_send(sock, probe, strlen(probe), 0) >= 0) {
            char resp[256];
            int rc = strata_msg_recv(sock, resp, sizeof(resp) - 1, 0);
            strata_sock_close(sock);
            if (rc > 0) { resp[rc] = '\0'; if (strstr(resp, "\"ok\":true")) return 1; }
        } else { strata_sock_close(sock); }
    }
    return 0;
}

static void write_pid_file(void) {
    FILE *f = fopen(PID_FILE, "w");
    if (f) { fprintf(f, "%d\n", getpid()); fclose(f); }
}

static void init_database(void) {
    strata_store *store = strata_store_open_sqlite(DB_PATH);
    if (!store) { fprintf(stderr, "cisco: failed to open %s\n", DB_PATH); exit(1); }
    strata_store_init(store);
    strata_repo_create(store, "town-hall", "Town Hall");
    strata_role_assign(store, "human", "villager", "town-hall");
    strata_role_assign(store, "claudette-service", "villager", "town-hall");
    strata_store_close(store);
}

static int read_api_key(char *key, int cap) {
    FILE *f = fopen("private/config.json", "r");
    if (!f) return 0;
    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = '\0';
    const char *section = strstr(buf, "\"anthropic\"");
    if (!section) return 0;
    const char *brace = strchr(section, '{');
    if (!brace) return 0;
    const char *kp = strstr(brace, "\"key\"");
    if (!kp) return 0;
    kp += 5;
    while (*kp == ' ' || *kp == '\t' || *kp == '\n') kp++;
    if (*kp != ':') return 0;
    kp++;
    while (*kp == ' ' || *kp == '\t' || *kp == '\n') kp++;
    if (*kp != '"') return 0;
    kp++;
    int pos = 0;
    while (*kp && *kp != '"' && pos < cap - 1) key[pos++] = *kp++;
    key[pos] = '\0';
    return pos;
}

static char anthropic_event[1024];
static char claudette_event[512];

int main(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGABRT, abort_handler);
    atexit(cleanup);

    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");

    char api_key[256] = {0};
    read_api_key(api_key, sizeof(api_key));

    fprintf(stderr, "cisco: initializing (key=%s)\n", api_key[0] ? "yes" : "NO");
    init_database();

    /* Fork store */
    fflush(stdout); fflush(stderr);
    store_pid = fork();
    if (store_pid < 0) { perror("fork store"); exit(1); }
    if (store_pid == 0) {
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
        _exit(store_service_run(DB_PATH, STORE_EP));
    }
    fprintf(stderr, "cisco: waiting for store...\n");
    if (!wait_for_service(STORE_EP, 30)) {
        fprintf(stderr, "cisco: store failed\n"); exit(1);
    }

    /* Register dens */
    host = strata_den_host_create();

    /* Anthropic — privileged JS den (needs network for bedrock.fetch) */
    if (strata_den_js_register(host, "anthropic", "dens/anthropic.js",
            NULL, STORE_EP, ANTHROPIC_PUB, ANTHROPIC_REP) != 0) {
        fprintf(stderr, "cisco: failed to register anthropic\n"); exit(1);
    }
    strata_den_set_privileged(host, "anthropic", 1);

    /* Library — sandboxed JS den, the village collection */
    if (strata_den_js_register(host, "library", "dens/library.js",
            NULL, STORE_EP, LIBRARY_PUB, LIBRARY_REP) != 0) {
        fprintf(stderr, "cisco: failed to register library\n"); exit(1);
    }

    /* Claudette — sandboxed JS den, talks to anthropic + library */
    if (strata_den_js_register(host, "claudette", "dens/claudette.js",
            NULL, STORE_EP, CLAUDETTE_PUB, CLAUDETTE_REP) != 0) {
        fprintf(stderr, "cisco: failed to register claudette\n"); exit(1);
    }
    strata_den_add_peer(host, "claudette", ANTHROPIC_REP);
    strata_den_add_peer(host, "claudette", LIBRARY_REP);

    /* Spawn anthropic with API key */
    char esc_key[512];
    json_escape(api_key, (int)strlen(api_key), esc_key, sizeof(esc_key));
    snprintf(anthropic_event, sizeof(anthropic_event),
        "{\"api_key\":\"%s\"}", esc_key);

    fflush(stdout); fflush(stderr);
    anthropic_pid = strata_den_spawn(host, "anthropic",
        anthropic_event, (int)strlen(anthropic_event));
    if (anthropic_pid <= 0) {
        fprintf(stderr, "cisco: failed to spawn anthropic\n"); exit(1);
    }
    usleep(500000);

    /* Spawn library */
    fflush(stdout); fflush(stderr);
    library_pid = strata_den_spawn(host, "library", "{}", 2);
    if (library_pid <= 0) {
        fprintf(stderr, "cisco: failed to spawn library\n"); exit(1);
    }
    usleep(500000);

    /* Spawn claudette */
    snprintf(claudette_event, sizeof(claudette_event),
        "{\"anthropic_ep\":\"%s\",\"library_ep\":\"%s\"}",
        ANTHROPIC_REP, LIBRARY_REP);

    fflush(stdout); fflush(stderr);
    claudette_pid = strata_den_spawn(host, "claudette",
        claudette_event, (int)strlen(claudette_event));
    if (claudette_pid <= 0) {
        fprintf(stderr, "cisco: failed to spawn claudette\n"); exit(1);
    }

    usleep(300000);
    write_pid_file();

    fprintf(stderr, "\n");
    fprintf(stderr, "  ┌─────────────────────────────────────┐\n");
    fprintf(stderr, "  │           cisco, utah                │\n");
    fprintf(stderr, "  │                                      │\n");
    fprintf(stderr, "  │  store:     %s    │\n", STORE_EP);
    fprintf(stderr, "  │  anthropic: %s    │\n", ANTHROPIC_REP);
    fprintf(stderr, "  │  library:   %s    │\n", LIBRARY_REP);
    fprintf(stderr, "  │  claudette: %s    │\n", CLAUDETTE_REP);
    fprintf(stderr, "  │                                      │\n");
    fprintf(stderr, "  │  population: 3                       │\n");
    fprintf(stderr, "  └─────────────────────────────────────┘\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  ./build_test/strata-human --endpoint %s --entity human \\\n", STORE_EP);
    fprintf(stderr, "    --agent claudette=%s --agent library=%s\n\n",
            CLAUDETTE_REP, LIBRARY_REP);

    while (running) {
        int status;
        pid_t died = waitpid(-1, &status, WNOHANG);
        if (died > 0) {
            fprintf(stderr, "cisco: child %d exited\n", died);
            if (died == store_pid) { store_pid = -1; running = 0; }
            if (died == anthropic_pid) {
                fprintf(stderr, "cisco: respawning anthropic\n");
                fflush(stderr);
                anthropic_pid = strata_den_spawn(host, "anthropic",
                    anthropic_event, (int)strlen(anthropic_event));
            }
            if (died == library_pid) {
                fprintf(stderr, "cisco: respawning library\n");
                fflush(stderr);
                library_pid = strata_den_spawn(host, "library", "{}", 2);
            }
            if (died == claudette_pid) {
                fprintf(stderr, "cisco: respawning claudette\n");
                fflush(stderr);
                claudette_pid = strata_den_spawn(host, "claudette",
                    claudette_event, (int)strlen(claudette_event));
            }
        }
        usleep(500000);
    }

    fprintf(stderr, "cisco: shutting down\n");
    return 0;
}
