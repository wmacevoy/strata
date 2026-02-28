#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include "strata/den.h"

#define TEST(name) printf("  %-50s", name)
#define PASS()     printf("PASS\n")

int main(void) {
    printf("test_den\n");

    /* Compile echo.wat -> echo.wasm if needed */
    const char *wasm_path = "/tmp/strata_echo.wasm";
    int rc = system("wat2wasm dens/echo.wat -o /tmp/strata_echo.wasm 2>&1");
    if (rc != 0) {
        fprintf(stderr, "wat2wasm failed (is wabt installed?)\n");
        return 1;
    }

    /* Create den host */
    TEST("create den host");
    strata_den_host *host = strata_den_host_create();
    assert(host != NULL);
    PASS();

    /* Register echo den */
    TEST("register echo den");
    rc = strata_den_register(host, "echo", wasm_path,
                               NULL, /* no trigger filter */
                               NULL, /* no SUB endpoint */
                               NULL  /* no REQ endpoint */);
    assert(rc == 0);
    PASS();

    /* Spawn echo den with event payload */
    TEST("spawn echo den (fork + WASM)");
    const char *event = "{\"change\":\"create\",\"repo_id\":\"proj-1\"}";
    pid_t pid = strata_den_spawn(host, "echo", event, (int)strlen(event));
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
    pid_t pid2 = strata_den_spawn(host, "echo", event2, (int)strlen(event2));
    assert(pid2 > 0);
    waited = waitpid(pid2, &status, 0);
    assert(waited == pid2);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    PASS();

    /* Test spawning unknown den */
    TEST("spawn unknown den returns -1");
    pid_t bad = strata_den_spawn(host, "nonexistent", "{}", 2);
    assert(bad == -1);
    PASS();

    /* Cleanup */
    strata_den_host_free(host);
    unlink(wasm_path);

    printf("ALL TESTS PASSED\n");
    return 0;
}
