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
#include <zmq.h>

extern int messenger_run(const char *endpoint, int timeout);

#define TEST(name) do { printf("  %-55s", name); fflush(stdout); } while(0)
#define PASS()     do { printf("PASS\n"); fflush(stdout); } while(0)

#define MESSENGER_EP "tcp://127.0.0.1:19592"

static pid_t messenger_pid = -1;
static void *zmq_ctx = NULL;
static void *client = NULL;

static void cleanup(void) {
    if (client) { zmq_close(client); client = NULL; }
    if (zmq_ctx) { zmq_ctx_destroy(zmq_ctx); zmq_ctx = NULL; }
    if (messenger_pid > 0) { kill(messenger_pid, SIGTERM); waitpid(messenger_pid, NULL, 0); messenger_pid = -1; }
}

static void abort_handler(int sig) {
    (void)sig;
    cleanup();
    _exit(1);
}

static int wait_for_service(const char *endpoint, int max_retries) {
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

    /* Set up ZMQ client */
    zmq_ctx = zmq_ctx_new();
    client = zmq_socket(zmq_ctx, ZMQ_REQ);
    int timeout = 30000;
    zmq_setsockopt(client, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_connect(client, MESSENGER_EP);
    usleep(100000);

    /* Test init */
    TEST("init action");
    {
        const char *req = "{\"action\":\"init\"}";
        zmq_send(client, req, strlen(req), 0);
        char resp[1024] = {0};
        int rc = zmq_recv(client, resp, sizeof(resp) - 1, 0);
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"name\":\"messenger\"") != NULL);
    }
    PASS();

    /* Test discover */
    TEST("discover action");
    {
        const char *req = "{\"action\":\"discover\"}";
        zmq_send(client, req, strlen(req), 0);
        char resp[4096] = {0};
        int rc = zmq_recv(client, resp, sizeof(resp) - 1, 0);
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"fetch\"") != NULL);
    }
    PASS();

    /* Test fetch — HTTP GET */
    TEST("fetch GET https://httpbin.org/get");
    {
        const char *req = "{\"action\":\"fetch\",\"url\":\"https://httpbin.org/get\"}";
        zmq_send(client, req, strlen(req), 0);
        char *resp = malloc(1024 * 1024);
        assert(resp);
        int rc = zmq_recv(client, resp, 1024 * 1024 - 1, 0);
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
        zmq_send(client, req, strlen(req), 0);
        char *resp = malloc(1024 * 1024);
        assert(resp);
        int rc = zmq_recv(client, resp, 1024 * 1024 - 1, 0);
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
        zmq_send(client, req, strlen(req), 0);
        char resp[4096] = {0};
        int rc = zmq_recv(client, resp, sizeof(resp) - 1, 0);
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
        zmq_send(client, req, strlen(req), 0);
        char resp[1024] = {0};
        int rc = zmq_recv(client, resp, sizeof(resp) - 1, 0);
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"name\":\"messenger\"") != NULL);
    }
    PASS();

    printf("ALL TESTS PASSED\n");
    return 0;
}
