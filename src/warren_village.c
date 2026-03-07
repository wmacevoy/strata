/*
 * Warren's Village — launcher for 3 agent dens + human entry point.
 *
 * Startup:
 *   1. Init SQLite schema, create repos, assign roles
 *   2. Fork store_service
 *   3. Register & spawn gee, inch, loom JS dens
 *   4. Block until SIGTERM/SIGINT
 *   5. Cleanup: kill children, reap, delete PID file
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <zmq.h>
#include "strata/store.h"
#include "strata/den.h"

extern int store_service_run(const char *db_path, const char *endpoint);
extern int code_smith_run(const char *endpoint, const char *root, int readonly);

#define DB_PATH       "/tmp/warren_village.db"
#define PID_FILE      "/tmp/warren_village.pid"
#define STORE_EP      "tcp://127.0.0.1:5560"
#define SMITH_EP      "tcp://127.0.0.1:5590"

#define GEE_REP       "tcp://127.0.0.1:5570"
#define INCH_REP      "tcp://127.0.0.1:5571"
#define LOOM_REP      "tcp://127.0.0.1:5572"

#define GEE_PUB       "tcp://127.0.0.1:5580"
#define INCH_PUB      "tcp://127.0.0.1:5581"
#define LOOM_PUB      "tcp://127.0.0.1:5582"

static volatile int running = 1;
static pid_t store_pid = -1;
static pid_t smith_pid = -1;
static pid_t gee_pid = -1;
static pid_t inch_pid = -1;
static pid_t loom_pid = -1;
static strata_den_host *host = NULL;

static void kill_child(pid_t *pid) {
    if (*pid > 0) {
        kill(*pid, SIGTERM);
        waitpid(*pid, NULL, 0);
        *pid = -1;
    }
}

static void cleanup(void) {
    kill_child(&gee_pid);
    kill_child(&inch_pid);
    kill_child(&loom_pid);
    if (host) { strata_den_host_free(host); host = NULL; }
    kill_child(&smith_pid);
    kill_child(&store_pid);
    unlink(PID_FILE);
}

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

static void abort_handler(int sig) {
    (void)sig;
    cleanup();
    _exit(1);
}

static int wait_for_store(const char *endpoint, int max_retries) {
    void *ctx = zmq_ctx_new();
    void *sock = zmq_socket(ctx, ZMQ_REQ);
    int timeout = 500;
    zmq_setsockopt(sock, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_setsockopt(sock, ZMQ_SNDTIMEO, &timeout, sizeof(timeout));
    int linger = 0;
    zmq_setsockopt(sock, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_connect(sock, endpoint);

    int ready = 0;
    for (int i = 0; i < max_retries && !ready; i++) {
        usleep(100000);
        const char *probe = "{\"action\":\"init\"}";
        if (zmq_send(sock, probe, strlen(probe), 0) >= 0) {
            char resp[256];
            int rc = zmq_recv(sock, resp, sizeof(resp) - 1, 0);
            if (rc > 0) {
                resp[rc] = '\0';
                if (strstr(resp, "\"ok\":true")) ready = 1;
            }
        }
    }

    zmq_close(sock);
    zmq_ctx_destroy(ctx);
    return ready;
}

static void write_pid_file(void) {
    FILE *f = fopen(PID_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
}

static void init_database(void) {
    strata_store *store = strata_store_open_sqlite(DB_PATH);
    if (!store) {
        fprintf(stderr, "village: failed to open %s\n", DB_PATH);
        exit(1);
    }
    strata_store_init(store);

    /* Create repos */
    strata_repo_create(store, "town-hall", "Town Hall");
    strata_repo_create(store, "gee", "gee's space");
    strata_repo_create(store, "inch", "inch's space");
    strata_repo_create(store, "loom", "loom's space");

    /* Everyone is a villager in town-hall */
    strata_role_assign(store, "warren", "villager", "town-hall");
    strata_role_assign(store, "gee", "villager", "town-hall");
    strata_role_assign(store, "inch", "villager", "town-hall");
    strata_role_assign(store, "loom", "villager", "town-hall");

    /* Each agent owns their own repo */
    strata_role_assign(store, "gee", "owner", "gee");
    strata_role_assign(store, "inch", "owner", "inch");
    strata_role_assign(store, "loom", "owner", "loom");

    /* Warren can observe agent repos */
    strata_role_assign(store, "warren", "observer", "gee");
    strata_role_assign(store, "warren", "observer", "inch");
    strata_role_assign(store, "warren", "observer", "loom");

    /* Agents need store entity names for blob operations */
    strata_role_assign(store, "gee-service", "villager", "town-hall");
    strata_role_assign(store, "inch-service", "villager", "town-hall");
    strata_role_assign(store, "loom-service", "villager", "town-hall");
    strata_role_assign(store, "gee-service", "owner", "gee");
    strata_role_assign(store, "inch-service", "owner", "inch");
    strata_role_assign(store, "loom-service", "owner", "loom");

    strata_store_close(store);
}

int main(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGABRT, abort_handler);
    atexit(cleanup);

    /* Clean slate */
    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");

    fprintf(stderr, "village: initializing database\n");
    init_database();

    /* Fork store service */
    fflush(stdout);
    fflush(stderr);
    store_pid = fork();
    if (store_pid < 0) { perror("fork store"); exit(1); }
    if (store_pid == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        _exit(store_service_run(DB_PATH, STORE_EP));
    }

    fprintf(stderr, "village: waiting for store on %s\n", STORE_EP);
    if (!wait_for_store(STORE_EP, 20)) {
        fprintf(stderr, "village: store failed to start\n");
        exit(1);
    }
    fprintf(stderr, "village: store ready\n");

    /* Fork code-smith vocation */
    fflush(stdout);
    fflush(stderr);
    smith_pid = fork();
    if (smith_pid < 0) { perror("fork smith"); exit(1); }
    if (smith_pid == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        _exit(code_smith_run(SMITH_EP, ".", 0));
    }

    fprintf(stderr, "village: waiting for code-smith on %s\n", SMITH_EP);
    if (!wait_for_store(SMITH_EP, 20)) {
        fprintf(stderr, "village: code-smith failed to start\n");
        exit(1);
    }
    fprintf(stderr, "village: code-smith ready\n");

    /* Register dens */
    host = strata_den_host_create();

    if (strata_den_js_register(host, "gee", "dens/gee.js",
            NULL, STORE_EP, GEE_PUB, GEE_REP) != 0) {
        fprintf(stderr, "village: failed to register gee\n");
        exit(1);
    }
    if (strata_den_js_register(host, "inch", "dens/inch.js",
            NULL, STORE_EP, INCH_PUB, INCH_REP) != 0) {
        fprintf(stderr, "village: failed to register inch\n");
        exit(1);
    }
    if (strata_den_js_register(host, "loom", "dens/loom.js",
            NULL, STORE_EP, LOOM_PUB, LOOM_REP) != 0) {
        fprintf(stderr, "village: failed to register loom\n");
        exit(1);
    }

    /* Spawn dens */
    fflush(stdout);
    fflush(stderr);

    gee_pid = strata_den_spawn(host, "gee", "{}", 2);
    if (gee_pid <= 0) { fprintf(stderr, "village: failed to spawn gee\n"); exit(1); }
    fprintf(stderr, "village: gee spawned (pid %d) REP=%s PUB=%s\n", gee_pid, GEE_REP, GEE_PUB);

    inch_pid = strata_den_spawn(host, "inch", "{}", 2);
    if (inch_pid <= 0) { fprintf(stderr, "village: failed to spawn inch\n"); exit(1); }
    fprintf(stderr, "village: inch spawned (pid %d) REP=%s PUB=%s\n", inch_pid, INCH_REP, INCH_PUB);

    loom_pid = strata_den_spawn(host, "loom", "{}", 2);
    if (loom_pid <= 0) { fprintf(stderr, "village: failed to spawn loom\n"); exit(1); }
    fprintf(stderr, "village: loom spawned (pid %d) REP=%s PUB=%s\n", loom_pid, LOOM_REP, LOOM_PUB);

    usleep(300000); /* let dens bind sockets */

    /* Write PID file and report */
    write_pid_file();
    fprintf(stderr, "\nvillage: ready\n");
    fprintf(stderr, "  store:      %s\n", STORE_EP);
    fprintf(stderr, "  code-smith: %s\n", SMITH_EP);
    fprintf(stderr, "  gee:        REP=%s PUB=%s\n", GEE_REP, GEE_PUB);
    fprintf(stderr, "  inch:       REP=%s PUB=%s\n", INCH_REP, INCH_PUB);
    fprintf(stderr, "  loom:       REP=%s PUB=%s\n", LOOM_REP, LOOM_PUB);
    fprintf(stderr, "\n");

    /* Block until signaled */
    while (running) {
        /* Reap any crashed children */
        int status;
        pid_t died = waitpid(-1, &status, WNOHANG);
        if (died > 0) {
            fprintf(stderr, "village: child %d exited\n", died);
            if (died == store_pid) { store_pid = -1; running = 0; }
            if (died == smith_pid) smith_pid = -1;
            if (died == gee_pid) gee_pid = -1;
            if (died == inch_pid) inch_pid = -1;
            if (died == loom_pid) loom_pid = -1;
        }
        usleep(500000);
    }

    fprintf(stderr, "village: shutting down\n");
    /* cleanup via atexit */
    return 0;
}
