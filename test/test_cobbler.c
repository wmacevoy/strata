/*
 * Integration test for the cobbler vocation (C → WASM compiler).
 *
 * 1. Start cobbler on a test port
 * 2. Test init, discover, compile (success + failure)
 * 3. Verify WASM output is valid (starts with \0asm magic)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <zmq.h>

extern int cobbler_run(const char *endpoint, const char *root, const char *clang);

#define TEST(name) do { printf("  %-55s", name); fflush(stdout); } while(0)
#define PASS()     do { printf("PASS\n"); fflush(stdout); } while(0)

#define COBBLER_EP "tcp://127.0.0.1:19591"

static pid_t cobbler_pid = -1;
static void *zmq_ctx = NULL;
static void *client = NULL;

static void cleanup(void) {
    if (client) { zmq_close(client); client = NULL; }
    if (zmq_ctx) { zmq_ctx_destroy(zmq_ctx); zmq_ctx = NULL; }
    if (cobbler_pid > 0) { kill(cobbler_pid, SIGTERM); waitpid(cobbler_pid, NULL, 0); cobbler_pid = -1; }
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

/* Base64 decode for verification */
static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static unsigned char *base64_decode(const char *str, size_t len, size_t *out_len) {
    size_t olen = len / 4 * 3;
    if (len > 0 && str[len-1] == '=') olen--;
    if (len > 1 && str[len-2] == '=') olen--;
    unsigned char *out = malloc(olen);
    if (!out) return NULL;
    size_t i, j;
    for (i = 0, j = 0; i + 3 < len; i += 4) {
        int a = b64_val(str[i]), b = b64_val(str[i+1]);
        int c = b64_val(str[i+2]), d = b64_val(str[i+3]);
        if (a < 0 || b < 0) break;
        unsigned int v = (unsigned int)((a << 18) | (b << 12));
        if (c >= 0) v |= (unsigned int)(c << 6);
        if (d >= 0) v |= (unsigned int)d;
        out[j++] = (v >> 16) & 0xFF;
        if (c >= 0 && j < olen) out[j++] = (v >> 8) & 0xFF;
        if (d >= 0 && j < olen) out[j++] = v & 0xFF;
    }
    *out_len = j;
    return out;
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

    /* Set up ZMQ client */
    zmq_ctx = zmq_ctx_new();
    client = zmq_socket(zmq_ctx, ZMQ_REQ);
    int timeout = 10000; /* 10s for compilation */
    zmq_setsockopt(client, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_connect(client, COBBLER_EP);
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
        assert(strstr(resp, "\"name\":\"cobbler\"") != NULL);
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
        assert(strstr(resp, "\"compile\"") != NULL);
        assert(strstr(resp, "\"compile_file\"") != NULL);
        assert(strstr(resp, "\"clang_version\"") != NULL);
    }
    PASS();

    /* Test compile — minimal valid WASM */
    TEST("compile valid C to WASM");
    {
        const char *req =
            "{\"action\":\"compile\",\"source\":"
            "\"__attribute__((export_name(\\\"add\\\"))) "
            "int add(int a, int b) { return a + b; }\"}";
        zmq_send(client, req, strlen(req), 0);
        char *resp = malloc(1024 * 1024);
        assert(resp);
        int rc = zmq_recv(client, resp, 1024 * 1024 - 1, 0);
        assert(rc > 0);
        resp[rc] = '\0';
        if (!strstr(resp, "\"ok\":true")) {
            fprintf(stderr, "\ncompile response: %s\n", resp);
        }
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"wasm\":\"") != NULL);
        assert(strstr(resp, "\"size\":") != NULL);

        /* Extract and verify WASM magic bytes */
        const char *wasm_start = strstr(resp, "\"wasm\":\"");
        assert(wasm_start);
        wasm_start += 8; /* skip "wasm":" */
        const char *wasm_end = strchr(wasm_start, '"');
        assert(wasm_end);
        size_t b64_len = (size_t)(wasm_end - wasm_start);

        size_t decoded_len = 0;
        unsigned char *decoded = base64_decode(wasm_start, b64_len, &decoded_len);
        assert(decoded);
        assert(decoded_len >= 4);
        /* WASM magic: \0asm */
        assert(decoded[0] == 0x00);
        assert(decoded[1] == 0x61); /* 'a' */
        assert(decoded[2] == 0x73); /* 's' */
        assert(decoded[3] == 0x6d); /* 'm' */
        free(decoded);
        free(resp);
    }
    PASS();

    /* Test compile with bad source */
    TEST("compile bad source returns error");
    {
        const char *req =
            "{\"action\":\"compile\",\"source\":"
            "\"this is not valid C code!!!\"}";
        zmq_send(client, req, strlen(req), 0);
        char resp[8192] = {0};
        int rc = zmq_recv(client, resp, sizeof(resp) - 1, 0);
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
        zmq_send(client, req, strlen(req), 0);
        char resp[1024] = {0};
        int rc = zmq_recv(client, resp, sizeof(resp) - 1, 0);
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":true") != NULL);
        assert(strstr(resp, "\"name\":\"cobbler\"") != NULL);
    }
    PASS();

    /* Test unknown action */
    TEST("unknown action returns error");
    {
        const char *req = "{\"action\":\"bogus\"}";
        zmq_send(client, req, strlen(req), 0);
        char resp[1024] = {0};
        int rc = zmq_recv(client, resp, sizeof(resp) - 1, 0);
        assert(rc > 0);
        assert(strstr(resp, "\"ok\":false") != NULL);
        assert(strstr(resp, "bogus") != NULL);
    }
    PASS();

    printf("ALL TESTS PASSED\n");
    return 0;
}
