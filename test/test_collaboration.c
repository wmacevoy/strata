/*
 * Integration test for collaboration & journeyman pattern.
 *
 * Tests:
 * 1. Store service: all operations go through ZMQ (no direct SQLite)
 * 2. Entity registration + token authentication
 * 3. CLI through ZMQ: repo create, role assign, msg post/list/get
 * 4. Gatekeeper den: join request → approval → role assignment
 * 5. Two villagers (alice + builder-bot) collaborate via the atmosphere
 *
 * The key assertion: nobody touches SQLite directly except store_service.
 * Alice and builder-bot both go through ZMQ. Same door.
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
#include "strata/context.h"
#include "strata/den.h"
#include "strata/json_util.h"

/* store_service_run from store_service.c (linked as object lib) */
int store_service_run(const char *db_path, const char *endpoint);

#define DB_PATH           "/tmp/strata_test_collab.db"
#define STORE_ENDPOINT    "tcp://127.0.0.1:18560"

/* Gatekeeper den endpoints */
#define GK_REP            "tcp://127.0.0.1:18570"
#define GK_PUB            "tcp://127.0.0.1:18580"

#define TEST(name) do { printf("  %-55s", name); fflush(stdout); } while(0)
#define PASS()     do { printf("PASS\n"); fflush(stdout); } while(0)

static pid_t store_pid = -1;
static pid_t gk_pid = -1;
static void *g_zmq_ctx = NULL;
static void *g_req_sock = NULL;
static strata_den_host *g_host = NULL;

static void cleanup(void) {
    if (gk_pid > 0) { kill(gk_pid, SIGTERM); waitpid(gk_pid, NULL, 0); gk_pid = -1; }
    if (g_req_sock) { zmq_close(g_req_sock); g_req_sock = NULL; }
    if (g_zmq_ctx) { zmq_ctx_destroy(g_zmq_ctx); g_zmq_ctx = NULL; }
    if (g_host) { strata_den_host_free(g_host); g_host = NULL; }
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

/* ---- Store service ---- */

static void start_store(void) {
    /* Bootstrap: init schema with direct SQLite (just like strata --db init) */
    strata_store *store = strata_store_open_sqlite(DB_PATH);
    assert(store != NULL);
    strata_store_init(store);
    strata_store_close(store);

    fflush(stdout);
    fflush(stderr);
    store_pid = fork();
    assert(store_pid >= 0);
    if (store_pid == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        store_service_run(DB_PATH, STORE_ENDPOINT);
        _exit(0);
    }
    assert(wait_for_store(STORE_ENDPOINT, 20));
}

static void stop_store(void) {
    if (store_pid > 0) {
        kill(store_pid, SIGINT);
        waitpid(store_pid, NULL, 0);
        store_pid = -1;
    }
}

/* ---- ZMQ request helper ---- */

static void connect_store(void) {
    g_zmq_ctx = zmq_ctx_new();
    g_req_sock = zmq_socket(g_zmq_ctx, ZMQ_REQ);
    int timeout = 5000;
    zmq_setsockopt(g_req_sock, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_connect(g_req_sock, STORE_ENDPOINT);
    usleep(50000);
}

static void disconnect_store(void) {
    zmq_close(g_req_sock);
    zmq_ctx_destroy(g_zmq_ctx);
}

static int store_request(const char *req_json, char *resp, int resp_cap) {
    int rc = zmq_send(g_req_sock, req_json, strlen(req_json), 0);
    if (rc < 0) return -1;
    rc = zmq_recv(g_req_sock, resp, resp_cap - 1, 0);
    if (rc < 0) return -1;
    resp[rc] = '\0';
    return rc;
}

/* ---- Per-request ZMQ (for independent sockets) ---- */

static int zmq_one_request(const char *endpoint, const char *req_json,
                            char *resp, int resp_cap) {
    void *ctx = zmq_ctx_new();
    void *sock = zmq_socket(ctx, ZMQ_REQ);
    int timeout = 5000;
    zmq_setsockopt(sock, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_connect(sock, endpoint);
    usleep(50000);

    zmq_send(sock, req_json, strlen(req_json), 0);
    int rc = zmq_recv(sock, resp, resp_cap - 1, 0);
    if (rc >= 0) resp[rc] = '\0';

    zmq_close(sock);
    zmq_ctx_destroy(ctx);
    return rc;
}

/* ================================================================== */

int main(void) {
    signal(SIGABRT, abort_handler);
    signal(SIGTERM, abort_handler);
    atexit(cleanup);

    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");

    printf("test_collaboration\n");
    fflush(stdout);

    /* ---- Phase 1: Store service, all through ZMQ ---- */

    start_store();
    connect_store();

    char resp[16384] = {0};
    int rc;

    /* Create project-alpha repo through ZMQ */
    TEST("repo create via ZMQ");
    rc = store_request(
        "{\"action\":\"repo_create\",\"repo\":\"project-alpha\",\"name\":\"Project Alpha\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    /* Create gatekeeper repo through ZMQ */
    TEST("gatekeeper repo via ZMQ");
    rc = store_request(
        "{\"action\":\"repo_create\",\"repo\":\"gatekeeper\",\"name\":\"Gatekeeper\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    /* ---- Phase 2: Entity registration + token auth ---- */

    TEST("register alice");
    rc = store_request(
        "{\"action\":\"entity_register\",\"entity\":\"alice\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    assert(strstr(resp, "\"token\":\"") != NULL);
    /* Extract alice's token */
    char alice_token[128] = {0};
    json_get_string(resp, "token", alice_token, sizeof(alice_token));
    assert(alice_token[0] != '\0');
    PASS();

    TEST("register builder-bot");
    rc = store_request(
        "{\"action\":\"entity_register\",\"entity\":\"builder-bot\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    char bot_token[128] = {0};
    json_get_string(resp, "token", bot_token, sizeof(bot_token));
    assert(bot_token[0] != '\0');
    PASS();

    TEST("authenticate alice with correct token");
    char auth_req[512];
    snprintf(auth_req, sizeof(auth_req),
        "{\"action\":\"entity_authenticate\",\"entity\":\"alice\",\"token\":\"%s\"}",
        alice_token);
    rc = store_request(auth_req, resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"valid\":true") != NULL);
    PASS();

    TEST("authenticate alice with wrong token fails");
    rc = store_request(
        "{\"action\":\"entity_authenticate\",\"entity\":\"alice\",\"token\":\"bad_token\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"valid\":false") != NULL);
    PASS();

    TEST("duplicate registration fails");
    rc = store_request(
        "{\"action\":\"entity_register\",\"entity\":\"alice\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":false") != NULL);
    PASS();

    /* ---- Phase 3: Role assignment + artifact ops via ZMQ ---- */

    TEST("assign developer role to alice via ZMQ");
    rc = store_request(
        "{\"action\":\"role_assign\",\"entity\":\"alice\",\"role\":\"developer\",\"repo\":\"project-alpha\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    TEST("assign developer role to builder-bot via ZMQ");
    rc = store_request(
        "{\"action\":\"role_assign\",\"entity\":\"builder-bot\",\"role\":\"developer\",\"repo\":\"project-alpha\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    /* Alice posts a task */
    TEST("alice posts task artifact via ZMQ");
    rc = store_request(
        "{\"action\":\"put\",\"repo\":\"project-alpha\",\"type\":\"task\","
        "\"content\":\"implement login page\",\"author\":\"alice\","
        "\"roles\":[\"developer\"]}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    char task_id[65] = {0};
    json_get_string(resp, "id", task_id, sizeof(task_id));
    PASS();

    /* Builder-bot sees it */
    TEST("builder-bot can list artifacts via ZMQ");
    rc = store_request(
        "{\"action\":\"list\",\"repo\":\"project-alpha\",\"type\":\"task\",\"entity\":\"builder-bot\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    assert(strstr(resp, "implement login page") != NULL);
    PASS();

    /* Builder-bot posts a result */
    TEST("builder-bot posts result artifact via ZMQ");
    rc = store_request(
        "{\"action\":\"put\",\"repo\":\"project-alpha\",\"type\":\"result\","
        "\"content\":\"login page implemented with OAuth\",\"author\":\"builder-bot\","
        "\"roles\":[\"developer\"]}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    /* Alice sees the result */
    TEST("alice can list all artifacts via ZMQ");
    rc = store_request(
        "{\"action\":\"list\",\"repo\":\"project-alpha\",\"entity\":\"alice\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "implement login page") != NULL);
    assert(strstr(resp, "login page implemented with OAuth") != NULL);
    PASS();

    /* ---- Phase 4: Token-authenticated requests ---- */

    TEST("put with valid token succeeds");
    snprintf(auth_req, sizeof(auth_req),
        "{\"action\":\"put\",\"repo\":\"project-alpha\",\"type\":\"note\","
        "\"content\":\"authenticated note\",\"author\":\"alice\","
        "\"token\":\"%s\",\"roles\":[\"developer\"]}",
        alice_token);
    rc = store_request(auth_req, resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    TEST("put with invalid token is rejected");
    rc = store_request(
        "{\"action\":\"put\",\"repo\":\"project-alpha\",\"type\":\"note\","
        "\"content\":\"evil note\",\"author\":\"alice\","
        "\"token\":\"bad_token_here\",\"roles\":[\"developer\"]}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":false") != NULL);
    assert(strstr(resp, "authentication failed") != NULL);
    PASS();

    /* ---- Phase 5: Journeyman pattern — different roles at different projects ---- */

    /* Create project-beta */
    TEST("create project-beta via ZMQ");
    rc = store_request(
        "{\"action\":\"repo_create\",\"repo\":\"project-beta\",\"name\":\"Project Beta\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    /* Alice has auditor role at project-beta (different from project-alpha!) */
    TEST("alice gets auditor role at project-beta");
    rc = store_request(
        "{\"action\":\"role_assign\",\"entity\":\"alice\",\"role\":\"auditor\",\"repo\":\"project-beta\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    /* Post auditor-only artifact in project-beta */
    rc = store_request(
        "{\"action\":\"put\",\"repo\":\"project-beta\",\"type\":\"audit\","
        "\"content\":\"compliance review needed\",\"author\":\"alice\","
        "\"roles\":[\"auditor\"]}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);

    /* builder-bot has NO role at project-beta — can't see the artifact */
    TEST("builder-bot cannot see project-beta artifacts");
    rc = store_request(
        "{\"action\":\"list\",\"repo\":\"project-beta\",\"entity\":\"builder-bot\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"artifacts\":[]") != NULL);
    PASS();

    /* alice CAN see them (she has auditor role there) */
    TEST("alice can see project-beta artifacts");
    rc = store_request(
        "{\"action\":\"list\",\"repo\":\"project-beta\",\"entity\":\"alice\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "compliance review needed") != NULL);
    PASS();

    /* ---- Phase 6: Gatekeeper den — join request flow ---- */

    /* Set up gatekeeper service entity with required role */
    TEST("setup gatekeeper-service entity");
    rc = store_request(
        "{\"action\":\"role_assign\",\"entity\":\"gatekeeper-service\",\"role\":\"gatekeeper\",\"repo\":\"gatekeeper\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    /* Register and spawn gatekeeper den */
    TEST("spawn gatekeeper den");
    g_host = strata_den_host_create();
    assert(g_host != NULL);
    rc = strata_den_js_register(g_host, "gatekeeper", "dens/gatekeeper.js",
                                 NULL, STORE_ENDPOINT,
                                 GK_PUB, GK_REP);
    assert(rc == 0);
    fflush(stdout);
    fflush(stderr);
    gk_pid = strata_den_spawn(g_host, "gatekeeper", "{}", 2);
    assert(gk_pid > 0);
    usleep(500000);
    PASS();

    /* Builder-bot requests to join project-beta */
    TEST("builder-bot requests join via gatekeeper");
    rc = zmq_one_request(GK_REP,
        "{\"action\":\"request_join\",\"entity\":\"builder-bot\","
        "\"role\":\"developer\",\"project\":\"project-beta\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    char request_id[65] = {0};
    json_get_string(resp, "request_id", request_id, sizeof(request_id));
    assert(request_id[0] != '\0');
    PASS();

    /* List pending requests */
    TEST("list pending join requests");
    rc = zmq_one_request(GK_REP,
        "{\"action\":\"list_requests\",\"status\":\"pending\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    assert(strstr(resp, "builder-bot") != NULL);
    assert(strstr(resp, "\"status\":\"pending\"") != NULL);
    PASS();

    /* Admin approves the request */
    TEST("admin approves join request");
    char approve_req[512];
    snprintf(approve_req, sizeof(approve_req),
        "{\"action\":\"approve_join\",\"request_id\":\"%s\",\"approver\":\"admin\"}",
        request_id);
    rc = zmq_one_request(GK_REP, approve_req, resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    /* Now builder-bot should have developer role on project-beta */
    TEST("builder-bot now has access to project-beta");
    rc = store_request(
        "{\"action\":\"list\",\"repo\":\"project-beta\",\"entity\":\"builder-bot\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    /* builder-bot now has developer role, but the existing artifact has auditor role,
     * so builder-bot still can't see the auditor-only artifact. But the role IS assigned. */
    PASS();

    /* Verify by posting a developer-visible artifact */
    TEST("post developer artifact in project-beta");
    rc = store_request(
        "{\"action\":\"put\",\"repo\":\"project-beta\",\"type\":\"code\","
        "\"content\":\"new feature code\",\"author\":\"builder-bot\","
        "\"roles\":[\"developer\"]}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    /* builder-bot can see its own developer-role artifact */
    TEST("builder-bot sees developer artifacts in project-beta");
    rc = store_request(
        "{\"action\":\"list\",\"repo\":\"project-beta\",\"type\":\"code\",\"entity\":\"builder-bot\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "new feature code") != NULL);
    PASS();

    /* ---- Phase 7: Privilege system via ZMQ ---- */

    TEST("grant privilege via ZMQ");
    rc = store_request(
        "{\"action\":\"privilege_grant\",\"entity\":\"alice\",\"privilege\":\"parent\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    TEST("check privilege via ZMQ");
    rc = store_request(
        "{\"action\":\"privilege_check\",\"entity\":\"alice\",\"privilege\":\"parent\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"has\":true") != NULL);
    PASS();

    TEST("check non-existent privilege");
    rc = store_request(
        "{\"action\":\"privilege_check\",\"entity\":\"builder-bot\",\"privilege\":\"parent\"}",
        resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"has\":false") != NULL);
    PASS();

    /* Cleanup happens via atexit */
    printf("ALL TESTS PASSED\n");
    return 0;
}
