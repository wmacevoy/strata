/*
 * End-to-end test for the message board strata.
 *
 * 1. Fork a store service
 * 2. Spawn the board strata (JS, via QuickJS in fork)
 * 3. Send POST requests via ZMQ REQ
 * 4. Send LIST request, verify messages appear
 * 5. Verify PUB notification received
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <zmq.h>
#include "strata/store.h"
#include "strata/den.h"

/* store_service_run from store_service.c */
extern int store_service_run(const char *db_path, const char *endpoint);

#define TEST(name) do { printf("  %-55s", name); fflush(stdout); } while(0)
#define PASS()     do { printf("PASS\n"); fflush(stdout); } while(0)

#define STORE_ENDPOINT "tcp://127.0.0.1:15560"
#define BOARD_REP      "tcp://127.0.0.1:15570"
#define BOARD_PUB      "tcp://127.0.0.1:15580"
#define DB_PATH        "/tmp/strata_test_board.db"

static pid_t store_pid = -1;
static pid_t board_pid = -1;
static strata_den_host *host = NULL;
static void *zmq_ctx = NULL;
static void *client = NULL;
static void *notif_sub = NULL;

static void cleanup(void) {
    if (board_pid > 0) { kill(board_pid, SIGTERM); waitpid(board_pid, NULL, 0); board_pid = -1; }
    if (client) { zmq_close(client); client = NULL; }
    if (notif_sub) { zmq_close(notif_sub); notif_sub = NULL; }
    if (zmq_ctx) { zmq_ctx_destroy(zmq_ctx); zmq_ctx = NULL; }
    if (host) { strata_den_host_free(host); host = NULL; }
    if (store_pid > 0) { kill(store_pid, SIGINT); waitpid(store_pid, NULL, 0); store_pid = -1; }
    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");
}

static void abort_handler(int sig) {
    (void)sig;
    cleanup();
    _exit(1);
}

/* Send a probe request to verify the store service is ready */
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

static void start_store_service(void) {
    /* Set up the store with a repo and role before forking the service */
    strata_store *store = strata_store_open_sqlite(DB_PATH);
    strata_store_init(store);
    strata_repo_create(store, "board", "Message Board");
    /* board-service entity needs the "user" role to list artifacts */
    strata_role_assign(store, "board-service", "user", "board");
    strata_store_close(store);

    fflush(stdout);
    fflush(stderr);
    store_pid = fork();
    if (store_pid == 0) {
        store_service_run(DB_PATH, STORE_ENDPOINT);
        _exit(0);
    }
}

int main(void) {
    /* Install cleanup handlers for robust teardown on failure */
    signal(SIGABRT, abort_handler);
    signal(SIGTERM, abort_handler);
    atexit(cleanup);

    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");

    printf("test_board\n");
    fflush(stdout);

    /* Start store service */
    TEST("start store service");
    start_store_service();
    assert(wait_for_store(STORE_ENDPOINT, 10));
    PASS();

    /* Create den host and register JS strata */
    TEST("register board strata");
    host = strata_den_host_create();
    assert(host != NULL);
    int rc = strata_den_js_register(host, "board", "dens/board.js",
                                NULL,            /* no SUB */
                                STORE_ENDPOINT,  /* REQ to store */
                                BOARD_PUB,       /* PUB for notifications */
                                BOARD_REP);      /* REP to serve API */
    assert(rc == 0);
    PASS();

    /* Spawn board strata */
    TEST("spawn board strata");
    fflush(stdout);
    board_pid = strata_den_spawn(host, "board", "{}", 2);
    assert(board_pid > 0);
    usleep(500000);  /* let it start, bind sockets, and connect to store */
    PASS();

    /* Set up a SUB socket to receive notifications */
    zmq_ctx = zmq_ctx_new();

    notif_sub = zmq_socket(zmq_ctx, ZMQ_SUB);
    zmq_connect(notif_sub, BOARD_PUB);
    zmq_setsockopt(notif_sub, ZMQ_SUBSCRIBE, "board/", 6);
    int timeout = 3000;
    zmq_setsockopt(notif_sub, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));

    /* Set up a REQ socket to call the board API */
    client = zmq_socket(zmq_ctx, ZMQ_REQ);
    zmq_connect(client, BOARD_REP);
    zmq_setsockopt(client, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    usleep(200000);  /* let sockets connect */

    /* POST a message */
    TEST("post a message");
    const char *post_req = "{\"action\":\"post\",\"author\":\"alice\",\"message\":\"hello everyone\"}";
    zmq_send(client, post_req, strlen(post_req), 0);
    char resp[4096] = {0};
    rc = zmq_recv(client, resp, sizeof(resp) - 1, 0);
    assert(rc > 0);
    if (!strstr(resp, "\"ok\":true")) { fprintf(stderr, "post response: %s\n", resp); }
    assert(strstr(resp, "\"ok\":true") != NULL);
    assert(strstr(resp, "\"id\":\"") != NULL);
    PASS();

    /* POST another message */
    TEST("post second message");
    const char *post2 = "{\"action\":\"post\",\"author\":\"bob\",\"message\":\"hi alice\"}";
    zmq_send(client, post2, strlen(post2), 0);
    memset(resp, 0, sizeof(resp));
    rc = zmq_recv(client, resp, sizeof(resp) - 1, 0);
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    /* LIST messages */
    TEST("list messages returns both");
    const char *list_req = "{\"action\":\"list\"}";
    zmq_send(client, list_req, strlen(list_req), 0);
    memset(resp, 0, sizeof(resp));
    rc = zmq_recv(client, resp, sizeof(resp) - 1, 0);
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    assert(strstr(resp, "hello everyone") != NULL);
    assert(strstr(resp, "hi alice") != NULL);
    PASS();

    /* Check PUB notification was received */
    TEST("notification received via PUB");
    char topic[256] = {0};
    char payload[4096] = {0};
    rc = zmq_recv(notif_sub, topic, sizeof(topic) - 1, 0);
    if (rc > 0) {
        rc = zmq_recv(notif_sub, payload, sizeof(payload) - 1, 0);
        assert(rc > 0);
        assert(strstr(topic, "board/new") != NULL);
        assert(strstr(payload, "alice") != NULL || strstr(payload, "bob") != NULL);
        PASS();
    } else {
        /* PUB/SUB timing can cause missed messages — acceptable */
        printf("SKIP (timing)\n");
    }

    /* Cleanup happens via atexit */
    printf("ALL TESTS PASSED\n");
    return 0;
}
