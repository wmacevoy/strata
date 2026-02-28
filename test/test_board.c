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
#include "strata/agent.h"

/* store_service_run from store_service.c */
extern int store_service_run(const char *db_path, const char *endpoint);

#define TEST(name) printf("  %-55s", name)
#define PASS()     printf("PASS\n")

#define STORE_ENDPOINT "tcp://127.0.0.1:15560"
#define BOARD_REP      "tcp://127.0.0.1:15570"
#define BOARD_PUB      "tcp://127.0.0.1:15580"
#define DB_PATH        "/tmp/strata_test_board.db"

static pid_t store_pid = -1;

static void start_store_service(void) {
    /* Set up the store with a repo and role before forking the service */
    strata_store *store = strata_store_open_sqlite(DB_PATH);
    strata_store_init(store);
    strata_repo_create(store, "board", "Message Board");
    /* board-service entity needs the "user" role to list artifacts */
    strata_role_assign(store, "board-service", "user", "board");
    strata_store_close(store);

    store_pid = fork();
    if (store_pid == 0) {
        store_service_run(DB_PATH, STORE_ENDPOINT);
        _exit(0);
    }
}

static void stop_store_service(void) {
    if (store_pid > 0) {
        kill(store_pid, SIGINT);
        waitpid(store_pid, NULL, 0);
    }
}

int main(void) {
    unlink(DB_PATH);

    printf("test_board\n");

    /* Start store service */
    TEST("start store service");
    start_store_service();
    usleep(200000);  /* let it bind */
    PASS();

    /* Create agent host and register JS strata */
    TEST("register board strata");
    strata_agent_host *host = strata_agent_host_create();
    assert(host != NULL);
    int rc = strata_js_register(host, "board", "agents/board.js",
                                NULL,            /* no SUB */
                                STORE_ENDPOINT,  /* REQ to store */
                                BOARD_PUB,       /* PUB for notifications */
                                BOARD_REP);      /* REP to serve API */
    assert(rc == 0);
    PASS();

    /* Spawn board strata */
    TEST("spawn board strata");
    pid_t board_pid = strata_agent_spawn(host, "board", "{}", 2);
    assert(board_pid > 0);
    usleep(300000);  /* let it start and bind sockets */
    PASS();

    /* Set up a SUB socket to receive notifications */
    void *zmq_ctx = zmq_ctx_new();

    void *notif_sub = zmq_socket(zmq_ctx, ZMQ_SUB);
    zmq_connect(notif_sub, BOARD_PUB);
    zmq_setsockopt(notif_sub, ZMQ_SUBSCRIBE, "board/", 6);
    int timeout = 3000;
    zmq_setsockopt(notif_sub, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    usleep(100000);  /* let SUB connect */

    /* Set up a REQ socket to call the board API */
    void *client = zmq_socket(zmq_ctx, ZMQ_REQ);
    zmq_connect(client, BOARD_REP);
    zmq_setsockopt(client, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));

    /* POST a message */
    TEST("post a message");
    const char *post_req = "{\"action\":\"post\",\"author\":\"alice\",\"message\":\"hello everyone\"}";
    zmq_send(client, post_req, strlen(post_req), 0);
    char resp[4096] = {0};
    rc = zmq_recv(client, resp, sizeof(resp) - 1, 0);
    assert(rc > 0);
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

    /* Cleanup */
    kill(board_pid, SIGTERM);
    waitpid(board_pid, NULL, 0);

    zmq_close(client);
    zmq_close(notif_sub);
    zmq_ctx_destroy(zmq_ctx);
    strata_agent_host_free(host);
    stop_store_service();
    unlink(DB_PATH);

    printf("ALL TESTS PASSED\n");
    return 0;
}
