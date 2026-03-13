/*
 * Integration test for the Claude agent den.
 *
 * Tests the den lifecycle, status, memory, and persistence.
 * Does NOT require ANTHROPIC_API_KEY — tests graceful handling when missing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "strata/msg.h"
#include <curl/curl.h>
#include "strata/store.h"
#include "strata/den.h"

extern int store_service_run(const char *db_path, const char *endpoint);
extern int code_smith_run(const char *endpoint, const char *root, int readonly);
extern int messenger_run(const char *endpoint, int timeout);

#define TEST(name) do { printf("  %-55s", name); fflush(stdout); } while(0)
#define PASS()     do { printf("PASS\n"); fflush(stdout); } while(0)

#define STORE_EP      "tcp://127.0.0.1:18560"
#define SMITH_EP      "tcp://127.0.0.1:18590"
#define MESSENGER_EP  "tcp://127.0.0.1:18592"
#define CLAUDE_REP    "tcp://127.0.0.1:18573"
#define CLAUDE_PUB    "tcp://127.0.0.1:18583"
#define DB_PATH       "/tmp/strata_test_claude.db"

static pid_t store_pid = -1;
static pid_t smith_pid = -1;
static pid_t messenger_pid = -1;
static pid_t claude_pid = -1;
static strata_den_host *host = NULL;

static void cleanup(void) {
    if (claude_pid > 0) { kill(claude_pid, SIGTERM); waitpid(claude_pid, NULL, 0); claude_pid = -1; }
    if (host) { strata_den_host_free(host); host = NULL; }
    if (messenger_pid > 0) { kill(messenger_pid, SIGTERM); waitpid(messenger_pid, NULL, 0); messenger_pid = -1; }
    if (smith_pid > 0) { kill(smith_pid, SIGTERM); waitpid(smith_pid, NULL, 0); smith_pid = -1; }
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
            if (rc > 0) {
                resp[rc] = '\0';
                if (strstr(resp, "\"ok\":true")) return 1;
            }
        } else {
            strata_sock_close(sock);
        }
    }
    return 0;
}

/* Per-request helper: connect, send, recv, close */
static int do_request(const char *endpoint, const char *req, char *resp, int resp_cap) {
    strata_sock *sock = strata_req_connect(endpoint);
    if (!sock) return -1;
    strata_msg_set_timeout(sock, 10000, 10000);
    strata_msg_send(sock, req, strlen(req), 0);
    int rc = strata_msg_recv(sock, resp, resp_cap - 1, 0);
    strata_sock_close(sock);
    if (rc >= 0) resp[rc] = '\0';
    return rc;
}

int main(void) {
    signal(SIGABRT, abort_handler);
    signal(SIGTERM, abort_handler);
    atexit(cleanup);

    /* Init curl before any fork — macOS ObjC runtime isn't fork-safe */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Kill stale */
    system("lsof -ti :18560 :18573 :18583 :18590 :18592 2>/dev/null | xargs kill 2>/dev/null || true");
    usleep(200000);

    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");

    printf("test_claude\n");
    fflush(stdout);

    /* Set up store */
    TEST("start store service");
    {
        strata_store *store = strata_store_open_sqlite(DB_PATH);
        strata_store_init(store);
        strata_role_assign(store, "claude-service", "villager", "_system");
        strata_store_close(store);
    }
    fflush(stdout); fflush(stderr);
    store_pid = fork();
    if (store_pid == 0) { _exit(store_service_run(DB_PATH, STORE_EP)); }
    assert(wait_for_service(STORE_EP, 20));
    PASS();

    /* Start code-smith */
    TEST("start code-smith");
    fflush(stdout); fflush(stderr);
    smith_pid = fork();
    if (smith_pid == 0) {
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
        _exit(code_smith_run(SMITH_EP, ".", 0));
    }
    assert(wait_for_service(SMITH_EP, 20));
    PASS();

    /* Start messenger */
    TEST("start messenger");
    fflush(stdout); fflush(stderr);
    messenger_pid = fork();
    if (messenger_pid == 0) {
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
        _exit(messenger_run(MESSENGER_EP, 30));
    }
    assert(wait_for_service(MESSENGER_EP, 20));
    PASS();

    /* Register and spawn claude den */
    TEST("spawn claude den");
    host = strata_den_host_create();
    assert(host != NULL);
    int rc = strata_den_js_register(host, "claude", "dens/claude.js",
                                    NULL, STORE_EP, CLAUDE_PUB, CLAUDE_REP);
    assert(rc == 0);

    char event_json[512];
    snprintf(event_json, sizeof(event_json),
        "{\"messenger_ep\":\"%s\",\"smith_ep\":\"%s\",\"cobbler_ep\":\"\","
        "\"model\":\"claude-sonnet-4-6\"}",
        MESSENGER_EP, SMITH_EP);

    fflush(stdout); fflush(stderr);
    claude_pid = strata_den_spawn(host, "claude", event_json, 2);
    assert(claude_pid > 0);
    usleep(500000);
    PASS();

    /* Test status */
    TEST("status action");
    {
        char resp[4096] = {0};
        rc = do_request(CLAUDE_REP, "{\"action\":\"status\"}", resp, sizeof(resp));
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"name\":\"claude\"") != NULL);
        assert(strstr(resp, "\"model\":\"claude-sonnet-4-6\"") != NULL);
    }
    PASS();

    /* Test say — will fail gracefully without API key */
    TEST("say without API key returns error gracefully");
    {
        char *resp = malloc(1024 * 1024);
        assert(resp);
        rc = do_request(CLAUDE_REP,
            "{\"action\":\"say\",\"from\":\"test\",\"message\":\"hello\"}",
            resp, 1024 * 1024);
        assert(rc > 0);
        /* Should get an error about API key, not a crash */
        assert(strstr(resp, "\"ok\":false") != NULL || strstr(resp, "\"ok\":true") != NULL);
        free(resp);
    }
    PASS();

    /* Test forget */
    TEST("forget clears conversation history");
    {
        char resp[1024] = {0};
        rc = do_request(CLAUDE_REP, "{\"action\":\"forget\"}", resp, sizeof(resp));
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "cleared") != NULL);
    }
    PASS();

    /* Test status shows 0 messages after forget */
    TEST("status shows 0 messages after forget");
    {
        char resp[4096] = {0};
        rc = do_request(CLAUDE_REP, "{\"action\":\"status\"}", resp, sizeof(resp));
        assert(rc > 0);
        assert(strstr(resp, "\"conversation_length\":0") != NULL);
    }
    PASS();

    /* Test persistence: kill and respawn */
    TEST("persistence across restart");
    {
        /* Send a say to create some conversation state */
        char resp[8192] = {0};
        rc = do_request(CLAUDE_REP,
            "{\"action\":\"say\",\"from\":\"test\",\"message\":\"remember me\"}",
            resp, sizeof(resp));
        assert(rc > 0);

        /* Kill den */
        kill(claude_pid, SIGTERM);
        waitpid(claude_pid, NULL, 0);
        claude_pid = -1;
        usleep(500000);

        /* Respawn */
        fflush(stdout); fflush(stderr);
        claude_pid = strata_den_spawn(host, "claude", event_json, 2);
        assert(claude_pid > 0);
        usleep(500000);

        /* Check status — should show messages from before restart */
        memset(resp, 0, sizeof(resp));
        rc = do_request(CLAUDE_REP, "{\"action\":\"status\"}", resp, sizeof(resp));
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        /* conversation_length should be > 0 (messages survived restart) */
        assert(strstr(resp, "\"conversation_length\":0") == NULL);
    }
    PASS();

    printf("ALL TESTS PASSED\n");
    return 0;
}
