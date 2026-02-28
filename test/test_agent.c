#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include "strata/agent.h"

#define TEST(name) printf("  %-50s", name)
#define PASS()     printf("PASS\n")

int main(void) {
    printf("test_agent\n");

    /* Compile echo.wat -> echo.wasm if needed */
    const char *wasm_path = "/tmp/strata_echo.wasm";
    int rc = system("wat2wasm agents/echo.wat -o /tmp/strata_echo.wasm 2>&1");
    if (rc != 0) {
        fprintf(stderr, "wat2wasm failed (is wabt installed?)\n");
        return 1;
    }

    /* Create agent host */
    TEST("create agent host");
    strata_agent_host *host = strata_agent_host_create();
    assert(host != NULL);
    PASS();

    /* Register echo agent */
    TEST("register echo agent");
    rc = strata_agent_register(host, "echo", wasm_path,
                               NULL, /* no trigger filter */
                               NULL, /* no SUB endpoint */
                               NULL  /* no REQ endpoint */);
    assert(rc == 0);
    PASS();

    /* Spawn echo agent with event payload */
    TEST("spawn echo agent (fork + WASM)");
    const char *event = "{\"change\":\"create\",\"repo_id\":\"proj-1\"}";
    pid_t pid = strata_agent_spawn(host, "echo", event, (int)strlen(event));
    assert(pid > 0);
    PASS();

    /* Wait for child to finish */
    TEST("child process completes successfully");
    int status;
    pid_t waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    PASS();

    /* Spawn a second instance to verify re-fork works (CoW) */
    TEST("spawn second instance (CoW reuse)");
    const char *event2 = "{\"change\":\"create\",\"repo_id\":\"proj-2\"}";
    pid_t pid2 = strata_agent_spawn(host, "echo", event2, (int)strlen(event2));
    assert(pid2 > 0);
    waited = waitpid(pid2, &status, 0);
    assert(waited == pid2);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    PASS();

    /* Test spawning unknown agent */
    TEST("spawn unknown agent returns -1");
    pid_t bad = strata_agent_spawn(host, "nonexistent", "{}", 2);
    assert(bad == -1);
    PASS();

    /* Cleanup */
    strata_agent_host_free(host);
    unlink(wasm_path);

    printf("ALL TESTS PASSED\n");
    return 0;
}
