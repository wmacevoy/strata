/*
 * Integration test for the claud-homestead den.
 *
 * 1. Clean up stale processes and files
 * 2. Start store service
 * 3. Set up claud repo + privileges
 * 4. Spawn claud-homestead den
 * 5. Test status, create_repo, init_homestead, deploy_den
 * 6. Kill den, respawn, verify local db restores state
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
#define DEN_DB         "/tmp/strata_den_claud-homestead.db"
#define STORE_ENDPOINT "tcp://127.0.0.1:19560"
#define CLAUD_REP      "tcp://127.0.0.1:19570"
#define CLAUD_PUB      "tcp://127.0.0.1:19580"

#define TEST(name) do { printf("  %-55s", name); fflush(stdout); } while(0)
#define PASS()     do { printf("PASS\n"); fflush(stdout); } while(0)

static pid_t store_pid = -1;
static pid_t claud_pid = -1;
static strata_den_host *host = NULL;

static void kill_stale_on_port(int port) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "lsof -ti :%d 2>/dev/null | xargs kill -9 2>/dev/null", port);
    system(cmd);
}

static void cleanup(void) {
    if (claud_pid > 0) { kill(claud_pid, SIGTERM); waitpid(claud_pid, NULL, 0); claud_pid = -1; }
    if (host) { strata_den_host_free(host); host = NULL; }
    if (store_pid > 0) { kill(store_pid, SIGINT); waitpid(store_pid, NULL, 0); store_pid = -1; }
    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");
    unlink(DEN_DB);
    unlink(DEN_DB "-wal");
    unlink(DEN_DB "-shm");
}

static void start_store(void) {
    strata_store *store = strata_store_open_sqlite(DB_PATH);
    assert(store != NULL);
    strata_store_init(store);

    /* Create claud repo and assign privileges */
    strata_repo_create(store, "claud", "Claud Homestead Builder");
    strata_role_assign(store, "claud-service", "core", "claud");
    strata_role_assign(store, "claud-service", "core", "_system");

    strata_store_close(store);

    fflush(stdout);
    fflush(stderr);
    store_pid = fork();
    assert(store_pid >= 0);
    if (store_pid == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        _exit(store_service_run(DB_PATH, STORE_ENDPOINT));
    }
    usleep(200000);
}

static int zmq_request(const char *endpoint, const char *req, char *resp, int resp_cap) {
    void *ctx = zmq_ctx_new();
    void *sock = zmq_socket(ctx, ZMQ_REQ);
    int timeout = 5000;
    zmq_setsockopt(sock, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    int linger = 0;
    zmq_setsockopt(sock, ZMQ_LINGER, &linger, sizeof(linger));
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
    /* Clean slate — kill stale processes and files */
    kill_stale_on_port(19560);
    kill_stale_on_port(19570);
    kill_stale_on_port(19580);
    usleep(100000);

    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");
    unlink(DEN_DB);
    unlink(DEN_DB "-wal");
    unlink(DEN_DB "-shm");

    atexit(cleanup);

    printf("test_claud_homestead\n");
    fflush(stdout);

    start_store();

    /* Register claud-homestead den */
    host = strata_den_host_create();
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
    fflush(stdout);
    fflush(stderr);
    claud_pid = strata_den_spawn(host, "claud-homestead", "{}", 2);
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

    /* Deploy a den to the homestead */
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
    claud_pid = -1;

    /* Respawn — local db should restore state automatically */
    TEST("respawn restores state from local db");
    fflush(stdout);
    fflush(stderr);
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

    /* Cleanup via atexit */
    kill(claud_pid, SIGTERM);
    waitpid(claud_pid, NULL, 0);
    claud_pid = -1;

    printf("ALL TESTS PASSED\n");
    return 0;
}
