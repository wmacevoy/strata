/*
 * Integration test for the claud-homestead den.
 *
 * 1. Start store service
 * 2. Set up claud repo + privileges
 * 3. Spawn claud-homestead den
 * 4. Test status, create_repo, init_homestead, pickle
 * 5. Kill den, respawn, verify unpickle restores state
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

extern int store_service_run(const char *db_path, const char *endpoint);

#define DB_PATH        "/tmp/strata_test_claud.db"
#define STORE_ENDPOINT "tcp://127.0.0.1:19560"
#define CLAUD_REP      "tcp://127.0.0.1:19570"
#define CLAUD_PUB      "tcp://127.0.0.1:19580"

#define TEST(name) do { printf("  %-55s", name); fflush(stdout); } while(0)
#define PASS()     do { printf("PASS\n"); fflush(stdout); } while(0)

static pid_t store_pid;

static void start_store(void) {
    strata_store *store = strata_store_open_sqlite(DB_PATH);
    assert(store != NULL);
    strata_store_init(store);

    /* Create claud repo and assign privileges */
    strata_repo_create(store, "claud", "Claud Homestead Builder");
    strata_role_assign(store, "claud-service", "core", "claud");
    strata_role_assign(store, "claud-service", "core", "_system");

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
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");

    printf("test_claud_homestead\n");
    fflush(stdout);

    start_store();

    /* Register claud-homestead den */
    strata_den_host *host = strata_den_host_create();
    assert(host != NULL);

    TEST("register claud-homestead den");
    int rc = strata_den_js_register(host, "claud-homestead",
                                     "dens/claud-homestead.js",
                                     NULL, STORE_ENDPOINT,
                                     CLAUD_PUB, CLAUD_REP);
    assert(rc == 0);
    PASS();

    /* Spawn */
    TEST("spawn claud-homestead den");
    pid_t claud_pid = strata_den_spawn(host, "claud-homestead", "{}", 2);
    assert(claud_pid > 0);
    usleep(400000);
    PASS();

    /* Test status — should be empty */
    char resp[8192] = {0};
    TEST("status returns empty state");
    rc = zmq_request(CLAUD_REP, "{\"action\":\"status\"}", resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    assert(strstr(resp, "\"homesteads\":[]") != NULL);
    PASS();

    /* Create a repo through the den */
    TEST("create_repo via den");
    memset(resp, 0, sizeof(resp));
    rc = zmq_request(CLAUD_REP,
        "{\"action\":\"create_repo\",\"repo\":\"testproject\",\"name\":\"Test Project\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    /* Init a homestead */
    TEST("init_homestead");
    memset(resp, 0, sizeof(resp));
    rc = zmq_request(CLAUD_REP,
        "{\"action\":\"init_homestead\",\"name\":\"dev\","
        "\"village_ep\":\"tcp://192.168.1.10:6000\","
        "\"store_ep\":\"tcp://192.168.1.10:5560\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    /* Deploy a den to the homestead (just records it) */
    TEST("deploy_den records deployment");
    memset(resp, 0, sizeof(resp));
    rc = zmq_request(CLAUD_REP,
        "{\"action\":\"deploy_den\",\"homestead\":\"dev\",\"den_name\":\"board\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    /* Status should now show the homestead */
    TEST("status shows configured homestead");
    memset(resp, 0, sizeof(resp));
    rc = zmq_request(CLAUD_REP, "{\"action\":\"status\"}", resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"name\":\"dev\"") != NULL);
    assert(strstr(resp, "\"board\"") != NULL);
    PASS();

    /* Pickle state */
    TEST("pickle saves state to store");
    memset(resp, 0, sizeof(resp));
    rc = zmq_request(CLAUD_REP, "{\"action\":\"pickle\"}", resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    /* Grant a privilege through the den */
    TEST("grant privilege via den");
    memset(resp, 0, sizeof(resp));
    rc = zmq_request(CLAUD_REP,
        "{\"action\":\"grant\",\"entity\":\"alice\",\"privilege\":\"vocation\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    /* Kill the den */
    kill(claud_pid, SIGTERM);
    waitpid(claud_pid, NULL, 0);

    /* Respawn — should unpickle saved state */
    TEST("respawn + unpickle restores state");
    claud_pid = strata_den_spawn(host, "claud-homestead", "{}", 2);
    assert(claud_pid > 0);
    usleep(400000);

    memset(resp, 0, sizeof(resp));
    rc = zmq_request(CLAUD_REP, "{\"action\":\"status\"}", resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    assert(strstr(resp, "\"name\":\"dev\"") != NULL);
    assert(strstr(resp, "\"board\"") != NULL);
    PASS();

    /* Cleanup */
    kill(claud_pid, SIGTERM);
    waitpid(claud_pid, NULL, 0);

    strata_den_host_free(host);
    stop_store();
    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");

    printf("ALL TESTS PASSED\n");
    return 0;
}
