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
#include "strata/transport.h"

extern int messenger_run(const char *endpoint, int timeout);

#define TEST(name) do { printf("  %-55s", name); fflush(stdout); } while(0)
#define PASS()     do { printf("PASS\n"); fflush(stdout); } while(0)

#define MESSENGER_EP "tcp://127.0.0.1:19592"

static pid_t messenger_pid = -1;
static strata_sock client;
static int client_open = 0;

static void cleanup(void) {
    if (client_open) { strata_sock_close(client); client_open = 0; }
    if (messenger_pid > 0) { kill(messenger_pid, SIGTERM); waitpid(messenger_pid, NULL, 0); messenger_pid = -1; }
}

static void abort_handler(int sig) {
    (void)sig;
    cleanup();
    _exit(1);
}

static int wait_for_service(const char *endpoint, int max_retries) {
    strata_sock sock;
    if (strata_req_open(&sock) != 0) return 0;
    strata_sock_set_recv_timeout(sock, 500);
    strata_sock_set_send_timeout(sock, 500);
    strata_sock_dial(sock, endpoint);

    int ready = 0;
    for (int i = 0; i < max_retries && !ready; i++) {
        usleep(100000);
        const char *probe = "{\"action\":\"init\"}";
        if (strata_send(sock, probe, strlen(probe)) >= 0) {
            char resp[256];
            int rc = strata_recv(sock, resp, sizeof(resp) - 1);
            if (rc > 0) {
                resp[rc] = '\0';
                if (strstr(resp, "\"ok\":true")) ready = 1;
            }
        }
    }

    strata_sock_close(sock);
    return ready;
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

    /* Set up client */
    assert(strata_req_open(&client) == 0);
    client_open = 1;
    strata_sock_set_recv_timeout(client, 30000);
    strata_sock_dial(client, MESSENGER_EP);
    usleep(100000);

    /* Test init */
    TEST("init action");
    {
        const char *req = "{\"action\":\"init\"}";
        strata_send(client, req, strlen(req));
        char resp[1024] = {0};
        int rc = strata_recv(client, resp, sizeof(resp) - 1);
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"name\":\"messenger\"") != NULL);
    }
    PASS();

    /* Test discover */
    TEST("discover action");
    {
        const char *req = "{\"action\":\"discover\"}";
        strata_send(client, req, strlen(req));
        char resp[4096] = {0};
        int rc = strata_recv(client, resp, sizeof(resp) - 1);
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"fetch\"") != NULL);
    }
    PASS();

    /* Test fetch — HTTP GET */
    TEST("fetch GET https://httpbin.org/get");
    {
        const char *req = "{\"action\":\"fetch\",\"url\":\"https://httpbin.org/get\"}";
        strata_send(client, req, strlen(req));
        char *resp = malloc(1024 * 1024);
        assert(resp);
        int rc = strata_recv(client, resp, 1024 * 1024 - 1);
        assert(rc > 0);
        resp[rc] = '\0';
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
        strata_send(client, req, strlen(req));
        char *resp = malloc(1024 * 1024);
        assert(resp);
        int rc = strata_recv(client, resp, 1024 * 1024 - 1);
        assert(rc > 0);
        resp[rc] = '\0';
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"status\":200") != NULL);
        free(resp);
    }
    PASS();

    /* Test fetch — bad URL */
    TEST("fetch bad URL returns error");
    {
        const char *req = "{\"action\":\"fetch\",\"url\":\"http://localhost:1/nonexistent\"}";
        strata_send(client, req, strlen(req));
        char resp[4096] = {0};
        int rc = strata_recv(client, resp, sizeof(resp) - 1);
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
        strata_send(client, req, strlen(req));
        char resp[1024] = {0};
        int rc = strata_recv(client, resp, sizeof(resp) - 1);
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"name\":\"messenger\"") != NULL);
    }
    PASS();

    printf("ALL TESTS PASSED\n");
    return 0;
}
