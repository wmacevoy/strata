/*
 * strata-homestead — containerized remote village.
 *
 * Runs a store service (forked child) and a village daemon (main process)
 * in a single process tree. Designed for Docker deployment.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <getopt.h>

#include "strata/store.h"
#include "strata/village.h"

/* Linked via store_service_lib object (STORE_SERVICE_NO_MAIN) */
extern int store_service_run(const char *db_path, const char *endpoint);

static pid_t store_pid = -1;

static const char *env_or(const char *var, const char *fallback) {
    const char *val = getenv(var);
    return (val && val[0]) ? val : fallback;
}

static void usage(void) {
    fprintf(stderr,
        "usage: strata-homestead [options]\n"
        "\n"
        "Options:\n"
        "  --db <path>          SQLite database path     (env: STRATA_DB_PATH)\n"
        "  --store-ep <ep>      Store service endpoint   (env: STRATA_STORE_ENDPOINT)\n"
        "  --village-ep <ep>    Village listen endpoint   (env: STRATA_VILLAGE_ENDPOINT)\n"
        "  --help               Show this message\n"
        "\n"
        "Defaults:\n"
        "  db:         /var/strata/strata.db\n"
        "  store-ep:   tcp://127.0.0.1:5560\n"
        "  village-ep: tcp://0.0.0.0:6000\n"
    );
}

int main(int argc, char **argv) {
    const char *db_path = NULL;
    const char *store_ep = NULL;
    const char *village_ep = NULL;

    static struct option long_opts[] = {
        {"db",         required_argument, NULL, 'd'},
        {"store-ep",   required_argument, NULL, 's'},
        {"village-ep", required_argument, NULL, 'v'},
        {"help",       no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:s:v:h", long_opts, NULL)) != -1) {
        switch (c) {
            case 'd': db_path = optarg; break;
            case 's': store_ep = optarg; break;
            case 'v': village_ep = optarg; break;
            case 'h': usage(); return 0;
            default:  usage(); return 1;
        }
    }

    /* Resolve: CLI > env > default */
    if (!db_path)    db_path    = env_or("STRATA_DB_PATH",         "/var/strata/strata.db");
    if (!store_ep)   store_ep   = env_or("STRATA_STORE_ENDPOINT",  "tcp://127.0.0.1:5560");
    if (!village_ep) village_ep = env_or("STRATA_VILLAGE_ENDPOINT", "tcp://0.0.0.0:6000");

    /* Phase 1: Initialize schema */
    fprintf(stderr, "homestead: initializing %s\n", db_path);
    strata_store *store = strata_store_open_sqlite(db_path);
    if (!store) {
        fprintf(stderr, "homestead: failed to open database: %s\n", db_path);
        return 1;
    }
    strata_store_init(store);
    strata_store_close(store);

    /* Phase 2: Fork store service */
    store_pid = fork();
    if (store_pid < 0) {
        perror("homestead: fork");
        return 1;
    }
    if (store_pid == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        _exit(store_service_run(db_path, store_ep));
    }

    fprintf(stderr, "homestead: store_service pid=%d on %s\n", store_pid, store_ep);
    usleep(200000);  /* let store bind */

    /* Phase 3: Run village daemon (blocks until SIGINT/SIGTERM) */
    fprintf(stderr, "homestead: village on %s\n", village_ep);
    int rc = strata_village_run(village_ep);

    /* Phase 4: Cleanup */
    fprintf(stderr, "homestead: shutting down\n");
    if (store_pid > 0) {
        kill(store_pid, SIGTERM);
        waitpid(store_pid, NULL, 0);
    }

    return rc;
}
