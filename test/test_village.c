#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <zmq.h>

#include "strata/store.h"
#include "strata/context.h"
#include "strata/den.h"
#include "strata/village.h"

/* store_service_run from store_service.c (linked as object lib) */
int store_service_run(const char *db_path, const char *endpoint);

#define DB_PATH           "/tmp/strata_test_village.db"
#define STORE_ENDPOINT    "tcp://127.0.0.1:17560"
#define VILLAGE_ENDPOINT  "tcp://127.0.0.1:17600"

/* Local clone endpoints */
#define LOCAL_BOARD_REP   "tcp://127.0.0.1:17570"
#define LOCAL_BOARD_PUB   "tcp://127.0.0.1:17580"

#define TEST(name) do { printf("  %-55s", name); fflush(stdout); } while(0)
#define PASS()     do { printf("PASS\n"); fflush(stdout); } while(0)

static pid_t store_pid;
static pid_t village_pid;

static void start_store(void) {
    strata_store *store = strata_store_open_sqlite(DB_PATH);
    assert(store != NULL);
    strata_store_init(store);
    strata_repo_create(store, "board", "Message Board");
    strata_role_assign(store, "board-service", "user", "board");
    strata_store_close(store);

    store_pid = fork();
    assert(store_pid >= 0);
    if (store_pid == 0) {
        store_service_run(DB_PATH, STORE_ENDPOINT);
        _exit(0);
    }
    usleep(200000);
}

static void stop_store(void) {
    kill(store_pid, SIGINT);
    waitpid(store_pid, NULL, 0);
}

static void start_village(void) {
    village_pid = fork();
    assert(village_pid >= 0);
    if (village_pid == 0) {
        strata_village_run(VILLAGE_ENDPOINT);
        _exit(0);
    }
    usleep(200000);
}

static void stop_village(void) {
    kill(village_pid, SIGINT);
    waitpid(village_pid, NULL, 0);
}

/* Send a JSON request via ZMQ REQ and get response */
static int zmq_request(const char *endpoint, const char *req, char *resp, int resp_cap) {
    void *ctx = zmq_ctx_new();
    void *sock = zmq_socket(ctx, ZMQ_REQ);
    int timeout = 5000;
    zmq_setsockopt(sock, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_connect(sock, endpoint);
    usleep(50000);

    zmq_send(sock, req, strlen(req), 0);
    int rc = zmq_recv(sock, resp, resp_cap - 1, 0);
    if (rc >= 0) resp[rc] = '\0';

    zmq_close(sock);
    zmq_ctx_destroy(ctx);
    return rc;
}

int main(void) {
    unlink(DB_PATH);
    /* Clean up WAL/SHM files from previous runs */
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");

    printf("test_village\n");
    fflush(stdout);

    start_store();

    /* Register board.js strata */
    strata_den_host *host = strata_den_host_create();
    assert(host != NULL);

    TEST("register board.js strata");
    int rc = strata_den_js_register(host, "board", "dens/board.js",
                                 NULL, STORE_ENDPOINT,
                                 LOCAL_BOARD_PUB, LOCAL_BOARD_REP);
    assert(rc == 0);
    PASS();

    /* ---- Test local clone ---- */

    TEST("local clone spawns board strata");
    pid_t local_pid = strata_clone(host, "board", "{}", 2);
    assert(local_pid > 0);
    usleep(300000);
    PASS();

    /* Quick sanity check: store PUT works directly */
    TEST("store service PUT works directly");
    char resp[4096] = {0};
    rc = zmq_request(STORE_ENDPOINT,
        "{\"action\":\"put\",\"repo\":\"board\",\"type\":\"message\","
        "\"content\":\"direct test\",\"author\":\"alice\",\"roles\":[\"user\"]}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    TEST("post message to local clone");
    memset(resp, 0, sizeof(resp));
    rc = zmq_request(LOCAL_BOARD_REP,
        "{\"action\":\"post\",\"author\":\"alice\",\"message\":\"hello from local\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    TEST("list messages from local clone");
    memset(resp, 0, sizeof(resp));
    rc = zmq_request(LOCAL_BOARD_REP,
        "{\"action\":\"list\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    assert(strstr(resp, "hello from local") != NULL);
    PASS();

    /* Kill the local board */
    kill(local_pid, SIGTERM);
    waitpid(local_pid, NULL, 0);

    /* ---- Test remote clone ---- */

    start_village();

    TEST("remote clone sends board to village");
    strata_clone_result result;
    memset(&result, 0, sizeof(result));
    rc = strata_remote_clone(host, "board", VILLAGE_ENDPOINT,
                              STORE_ENDPOINT, "{}", 2, &result);
    assert(rc == 0);
    assert(result.ok == 1);
    assert(result.den_rep[0] != '\0');
    usleep(300000);
    PASS();

    TEST("post message to remote clone");
    memset(resp, 0, sizeof(resp));
    rc = zmq_request(result.den_rep,
        "{\"action\":\"post\",\"author\":\"bob\",\"message\":\"hello from remote\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    TEST("list messages from remote clone shows both");
    memset(resp, 0, sizeof(resp));
    rc = zmq_request(result.den_rep,
        "{\"action\":\"list\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    assert(strstr(resp, "hello from local") != NULL);
    assert(strstr(resp, "hello from remote") != NULL);
    PASS();

    /* Verify origin store has both messages */
    TEST("origin store has both messages");
    memset(resp, 0, sizeof(resp));
    void *ctx = zmq_ctx_new();
    void *req = zmq_socket(ctx, ZMQ_REQ);
    int timeout = 5000;
    zmq_setsockopt(req, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_connect(req, STORE_ENDPOINT);
    usleep(50000);

    const char *list_req = "{\"action\":\"list\",\"repo\":\"board\",\"type\":\"message\",\"entity\":\"board-service\"}";
    zmq_send(req, list_req, strlen(list_req), 0);
    rc = zmq_recv(req, resp, sizeof(resp) - 1, 0);
    assert(rc > 0);
    resp[rc] = '\0';
    assert(strstr(resp, "hello from local") != NULL);
    assert(strstr(resp, "hello from remote") != NULL);
    zmq_close(req);
    zmq_ctx_destroy(ctx);
    PASS();

    /* Cleanup */
    strata_den_host_free(host);
    stop_village();
    stop_store();
    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");

    printf("ALL TESTS PASSED\n");
    return 0;
}
