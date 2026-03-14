/*
 * Integration test for the anthropic vocation.
 *
 * 1. Start anthropic on a test port
 * 2. Test init, discover, models
 * 3. Test ask with a real API call (if key available)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "strata/msg.h"
#include "strata/json_util.h"

extern int anthropic_run(const char *endpoint, int timeout);

#define TEST(name) do { printf("  %-55s", name); fflush(stdout); } while(0)
#define PASS()     do { printf("PASS\n"); fflush(stdout); } while(0)

#define ANTHROPIC_EP "tcp://127.0.0.1:19593"

static pid_t anthropic_pid = -1;

static void cleanup(void) {
    if (anthropic_pid > 0) { kill(anthropic_pid, SIGTERM); waitpid(anthropic_pid, NULL, 0); anthropic_pid = -1; }
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
            char resp[512];
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

static int do_request(const char *endpoint, const char *req, char *resp, int resp_cap) {
    strata_sock *sock = strata_req_connect(endpoint);
    if (!sock) return -1;
    strata_msg_set_timeout(sock, 60000, 60000);
    strata_msg_send(sock, req, strlen(req), 0);
    int rc = strata_msg_recv(sock, resp, resp_cap - 1, 0);
    strata_sock_close(sock);
    if (rc >= 0) resp[rc] = '\0';
    return rc;
}

/* Read API key from private/config.json and set as env var
 * so the forked anthropic_run picks it up */
static int load_key_to_env(void) {
    FILE *f = fopen("private/config.json", "r");
    if (!f) return 0;
    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = '\0';

    const char *section = strstr(buf, "\"anthropic\"");
    if (!section) return 0;
    const char *brace = strchr(section, '{');
    if (!brace) return 0;
    const char *kp = strstr(brace, "\"key\"");
    if (!kp) return 0;
    kp += 5;
    while (*kp == ' ' || *kp == '\t' || *kp == '\n') kp++;
    if (*kp != ':') return 0;
    kp++;
    while (*kp == ' ' || *kp == '\t' || *kp == '\n') kp++;
    if (*kp != '"') return 0;
    kp++;

    char key[256] = {0};
    int pos = 0;
    while (*kp && *kp != '"' && pos < 255) {
        key[pos++] = *kp++;
    }
    key[pos] = '\0';

    if (pos > 0) {
        setenv("ANTHROPIC_API_KEY", key, 1);
        return 1;
    }
    return 0;
}

int main(void) {
    signal(SIGABRT, abort_handler);
    signal(SIGTERM, abort_handler);
    atexit(cleanup);

    printf("test_anthropic\n");
    fflush(stdout);

    system("lsof -ti :19593 2>/dev/null | xargs kill 2>/dev/null || true");
    usleep(200000);

    /* Load key from config into env */
    int key_loaded = load_key_to_env();
    if (key_loaded)
        fprintf(stderr, "test_anthropic: loaded key from private/config.json\n");

    /* Start anthropic vocation */
    TEST("start anthropic vocation");
    fflush(stdout);
    fflush(stderr);
    anthropic_pid = fork();
    assert(anthropic_pid >= 0);
    if (anthropic_pid == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        _exit(anthropic_run(ANTHROPIC_EP, 30));
    }
    assert(wait_for_service(ANTHROPIC_EP, 20));
    PASS();

    /* Test init */
    TEST("init action");
    char resp[65536];
    int rc = do_request(ANTHROPIC_EP, "{\"action\":\"init\"}", resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true"));
    assert(strstr(resp, "\"anthropic\""));
    int has_key = strstr(resp, "\"has_key\":true") != NULL;
    printf("PASS (key=%s)\n", has_key ? "yes" : "no");

    /* Test discover */
    TEST("discover action");
    rc = do_request(ANTHROPIC_EP, "{\"action\":\"discover\"}", resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true"));
    assert(strstr(resp, "\"ask\""));
    PASS();

    /* Test models */
    TEST("models action");
    rc = do_request(ANTHROPIC_EP, "{\"action\":\"models\"}", resp, sizeof(resp));
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true"));
    assert(strstr(resp, "claude-sonnet"));
    PASS();

    /* Test ask — only if we have a key */
    if (has_key) {
        TEST("ask action (real API call)");
        const char *ask_req =
            "{\"action\":\"ask\","
            "\"messages\":[{\"role\":\"user\",\"content\":\"Say hi in 5 words.\"}],"
            "\"max_tokens\":60}";
        rc = do_request(ANTHROPIC_EP, ask_req, resp, sizeof(resp));
        assert(rc > 0);
        if (strstr(resp, "\"ok\":true")) {
            printf("PASS\n");
            /* Extract and show the response text */
            char *text = strstr(resp, "text");
            if (text) printf("    → %.200s\n", text);
        } else {
            printf("FAIL\n    → %.*s\n", rc > 300 ? 300 : rc, resp);
        }
    } else {
        TEST("ask action (skipped — no API key)");
        PASS();
    }

    printf("ALL TESTS PASSED\n");
    return 0;
}
