/*
 * Integration test for the cobbler vocation (C source validator via TCC).
 *
 * 1. Start cobbler on a test port
 * 2. Test init, discover, compile (success + failure)
 * 3. Verify TCC validates source and detects entry points
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "strata/msg.h"

extern int cobbler_run(const char *endpoint, const char *root, const char *clang);

#define TEST(name) do { printf("  %-55s", name); fflush(stdout); } while(0)
#define PASS()     do { printf("PASS\n"); fflush(stdout); } while(0)

#define COBBLER_EP "tcp://127.0.0.1:19591"

static pid_t cobbler_pid = -1;

static void cleanup(void) {
    if (cobbler_pid > 0) { kill(cobbler_pid, SIGTERM); waitpid(cobbler_pid, NULL, 0); cobbler_pid = -1; }
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

    printf("test_cobbler\n");
    fflush(stdout);

    /* Kill any stale process on our test port */
    system("lsof -ti :19591 2>/dev/null | xargs kill 2>/dev/null || true");
    usleep(200000);

    /* Start cobbler */
    TEST("start cobbler service");
    fflush(stdout);
    fflush(stderr);
    cobbler_pid = fork();
    if (cobbler_pid == 0) {
        _exit(cobbler_run(COBBLER_EP, ".", NULL));
    }
    assert(cobbler_pid > 0);
    assert(wait_for_service(COBBLER_EP, 20));
    PASS();

    /* Test init */
    TEST("init action");
    {
        char resp[1024] = {0};
        int rc = do_request(COBBLER_EP, "{\"action\":\"init\"}", resp, sizeof(resp));
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"name\":\"cobbler\"") != NULL);
    }
    PASS();

    /* Test discover */
    TEST("discover action");
    {
        char resp[4096] = {0};
        int rc = do_request(COBBLER_EP, "{\"action\":\"discover\"}", resp, sizeof(resp));
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"compile\"") != NULL);
        assert(strstr(resp, "\"compile_file\"") != NULL);
        assert(strstr(resp, "\"compiler\":\"tcc\"") != NULL);
    }
    PASS();

    /* Test compile — valid C with entry points */
    TEST("compile valid C source");
    {
        const char *req =
            "{\"action\":\"compile\",\"source\":"
            "\"int add(int a, int b) { return a + b; }\\n"
            "void on_event(const char *e, int l) { (void)e; (void)l; }\"}";
        char resp[8192] = {0};
        int rc = do_request(COBBLER_EP, req, resp, sizeof(resp));
        assert(rc > 0);
        if (!strstr(resp, "\"ok\":true")) {
            fprintf(stderr, "\ncompile response: %s\n", resp);
        }
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"valid\":true") != NULL);
        assert(strstr(resp, "\"has_on_event\":true") != NULL);
    }
    PASS();

    /* Test compile with bad source */
    TEST("compile bad source returns error");
    {
        const char *req =
            "{\"action\":\"compile\",\"source\":"
            "\"this is not valid C code!!!\"}";
        char resp[8192] = {0};
        int rc = do_request(COBBLER_EP, req, resp, sizeof(resp));
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":false") != NULL);
        assert(strstr(resp, "\"error\"") != NULL);
    }
    PASS();

    /* Test say action (talk unwrapping) */
    TEST("say action dispatches JSON");
    {
        const char *req =
            "{\"action\":\"say\",\"message\":\"{\\\"action\\\":\\\"init\\\"}\"}";
        char resp[1024] = {0};
        int rc = do_request(COBBLER_EP, req, resp, sizeof(resp));
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"name\":\"cobbler\"") != NULL);
    }
    PASS();

    /* Test unknown action */
    TEST("unknown action returns error");
    {
        char resp[1024] = {0};
        int rc = do_request(COBBLER_EP, "{\"action\":\"bogus\"}", resp, sizeof(resp));
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":false") != NULL);
        assert(strstr(resp, "bogus") != NULL);
    }
    PASS();

    printf("ALL TESTS PASSED\n");
    return 0;
}
