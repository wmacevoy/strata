/*
 * Integration test for the messenger vocation (HTTP client).
 *
 * 1. Start messenger on a test port
 * 2. Test init, discover
 * 3. Test fetch with a real HTTP GET
 * 4. Test error handling
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "strata/msg.h"

extern int messenger_run(const char *endpoint, int timeout);

#define TEST(name) do { printf("  %-55s", name); fflush(stdout); } while(0)
#define PASS()     do { printf("PASS\n"); fflush(stdout); } while(0)

#define MESSENGER_EP "tcp://127.0.0.1:19592"

static pid_t messenger_pid = -1;

static void cleanup(void) {
    if (messenger_pid > 0) { kill(messenger_pid, SIGTERM); waitpid(messenger_pid, NULL, 0); messenger_pid = -1; }
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
    strata_msg_set_timeout(sock, 30000, 30000);
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

    printf("test_messenger\n");
    fflush(stdout);

    /* Kill stale */
    system("lsof -ti :19592 2>/dev/null | xargs kill 2>/dev/null || true");
    usleep(200000);

    /* Start messenger — set OBJC_DISABLE_INITIALIZE_FORK_SAFETY for macOS curl+fork */
    TEST("start messenger service");
    setenv("OBJC_DISABLE_INITIALIZE_FORK_SAFETY", "YES", 1);
    fflush(stdout);
    fflush(stderr);
    messenger_pid = fork();
    if (messenger_pid == 0) {
        _exit(messenger_run(MESSENGER_EP, 30));
    }
    assert(messenger_pid > 0);
    assert(wait_for_service(MESSENGER_EP, 20));
    PASS();

    /* Test init */
    TEST("init action");
    {
        char resp[1024] = {0};
        int rc = do_request(MESSENGER_EP, "{\"action\":\"init\"}", resp, sizeof(resp));
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"name\":\"messenger\"") != NULL);
    }
    PASS();

    /* Test discover */
    TEST("discover action");
    {
        char resp[4096] = {0};
        int rc = do_request(MESSENGER_EP, "{\"action\":\"discover\"}", resp, sizeof(resp));
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"fetch\"") != NULL);
    }
    PASS();

    /* Test fetch — HTTP GET */
    TEST("fetch GET https://httpbin.org/get");
    {
        char *resp = malloc(1024 * 1024);
        assert(resp);
        int rc = do_request(MESSENGER_EP,
            "{\"action\":\"fetch\",\"url\":\"https://httpbin.org/get\"}",
            resp, 1024 * 1024);
        assert(rc > 0);
        if (!strstr(resp, "\"ok\":true")) {
            fprintf(stderr, "\nfetch response: %.200s\n", resp);
        }
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"status\":200") != NULL);
        assert(strstr(resp, "\"body\":\"") != NULL);
        free(resp);
    }
    PASS();

    /* Test fetch — POST with body and headers */
    TEST("fetch POST with body and headers");
    {
        const char *req =
            "{\"action\":\"fetch\","
            "\"url\":\"https://httpbin.org/post\","
            "\"method\":\"POST\","
            "\"headers\":[\"Content-Type: application/json\",\"X-Test: strata\"],"
            "\"body\":\"{\\\"hello\\\":\\\"world\\\"}\"}";
        char *resp = malloc(1024 * 1024);
        assert(resp);
        int rc = do_request(MESSENGER_EP, req, resp, 1024 * 1024);
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"status\":200") != NULL);
        free(resp);
    }
    PASS();

    /* Test fetch — bad URL */
    TEST("fetch bad URL returns error");
    {
        char resp[4096] = {0};
        int rc = do_request(MESSENGER_EP,
            "{\"action\":\"fetch\",\"url\":\"http://localhost:1/nonexistent\"}",
            resp, sizeof(resp));
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":false") != NULL);
        assert(strstr(resp, "\"error\"") != NULL);
    }
    PASS();

    /* Test say (talk unwrapping) */
    TEST("say action dispatches JSON");
    {
        const char *req =
            "{\"action\":\"say\",\"message\":\"{\\\"action\\\":\\\"init\\\"}\"}";
        char resp[1024] = {0};
        int rc = do_request(MESSENGER_EP, req, resp, sizeof(resp));
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"name\":\"messenger\"") != NULL);
    }
    PASS();

    printf("ALL TESTS PASSED\n");
    return 0;
}
