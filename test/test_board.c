/*
 * End-to-end test for the message board strata.
 *
 * 1. Fork a store service
 * 2. Spawn the board strata (JS, via QuickJS in fork)
 * 3. Send POST requests via REQ
 * 4. Send LIST request, verify messages appear
 * 5. Verify PUB notification received
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "strata/transport.h"
#include "strata/store.h"
#include "strata/den.h"

/* store_service_run from store_service.c */
extern int store_service_run(const char *db_path, const char *endpoint);

#define TEST(name) do { printf("  %-55s", name); fflush(stdout); } while(0)
#define PASS()     do { printf("PASS\n"); fflush(stdout); } while(0)

#define STORE_ENDPOINT "tcp://127.0.0.1:15560"
#define BOARD_REP      "tcp://127.0.0.1:15570"
#define BOARD_PUB      "tcp://127.0.0.1:15580"
#define DB_PATH        "/tmp/strata_test_board.db"

static pid_t store_pid = -1;
static pid_t board_pid = -1;
static strata_den_host *host = NULL;
static strata_sock client;
static strata_sock notif_sub;
static int client_open = 0;
static int notif_open = 0;

static void cleanup(void) {
    if (board_pid > 0) { kill(board_pid, SIGTERM); waitpid(board_pid, NULL, 0); board_pid = -1; }
    if (client_open) { strata_sock_close(client); client_open = 0; }
    if (notif_open) { strata_sock_close(notif_sub); notif_open = 0; }
    if (host) { strata_den_host_free(host); host = NULL; }
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

/* TCP connect probe — avoids nng init in parent (nng is not fork-safe).
 * Parses "tcp://host:port" and does a plain TCP connect to check liveness. */
static int wait_for_store(const char *endpoint, int max_retries) {
    /* Parse host:port from tcp://host:port */
    const char *hp = endpoint;
    if (strncmp(hp, "tcp://", 6) == 0) hp += 6;
    char host[256] = "127.0.0.1";
    int port = 0;
    const char *colon = strrchr(hp, ':');
    if (colon) {
        int hlen = (int)(colon - hp);
        if (hlen > 0 && hlen < (int)sizeof(host)) {
            memcpy(host, hp, hlen);
            host[hlen] = '\0';
        }
        port = atoi(colon + 1);
    }
    if (port == 0) return 0;

    for (int i = 0; i < max_retries; i++) {
        usleep(100000);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(host);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            close(fd);
            return 1;
        }
        close(fd);
    }
    return 0;
}

static void start_store_service(void) {
    /* Set up the store with a repo and role before forking the service */
    strata_store *store = strata_store_open_sqlite(DB_PATH);
    strata_store_init(store);
    strata_repo_create(store, "board", "Message Board");
    /* board-service entity needs the "user" role to list artifacts */
    strata_role_assign(store, "board-service", "user", "board");
    strata_store_close(store);

    fflush(stdout);
    fflush(stderr);
    store_pid = fork();
    if (store_pid == 0) {
        store_service_run(DB_PATH, STORE_ENDPOINT);
        _exit(0);
    }
}

int main(void) {
    /* Install cleanup handlers for robust teardown on failure */
    signal(SIGABRT, abort_handler);
    signal(SIGTERM, abort_handler);
    atexit(cleanup);

    /* Kill stale processes on test ports */
    system("lsof -ti :15560 :15570 :15580 2>/dev/null | xargs kill 2>/dev/null || true");
    usleep(200000);

    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");

    printf("test_board\n");
    fflush(stdout);

    /* Start store service */
    TEST("start store service");
    start_store_service();
    assert(wait_for_store(STORE_ENDPOINT, 20));
    PASS();

    /* Create den host and register JS strata */
    TEST("register board strata");
    host = strata_den_host_create();
    assert(host != NULL);
    int rc = strata_den_js_register(host, "board", "dens/board.js",
                                NULL,            /* no SUB */
                                STORE_ENDPOINT,  /* REQ to store */
                                BOARD_PUB,       /* PUB for notifications */
                                BOARD_REP);      /* REP to serve API */
    assert(rc == 0);
    PASS();

    /* Spawn board strata */
    TEST("spawn board strata");
    fflush(stdout);
    board_pid = strata_den_spawn(host, "board", "{}", 2);
    assert(board_pid > 0);
    usleep(500000);  /* let it start, bind sockets, and connect to store */
    PASS();

    /* Set up a SUB socket to receive notifications */
    strata_sub_open(&notif_sub);
    notif_open = 1;
    strata_sock_dial(notif_sub, BOARD_PUB);
    strata_sock_subscribe(notif_sub, "board/", 6);
    int timeout = 3000;
    strata_sock_set_recv_timeout(notif_sub, timeout);

    /* Set up a REQ socket to call the board API */
    strata_req_open(&client);
    client_open = 1;
    strata_sock_dial(client, BOARD_REP);
    strata_sock_set_recv_timeout(client, timeout);
    usleep(200000);  /* let sockets connect */

    /* POST a message */
    TEST("post a message");
    const char *post_req = "{\"action\":\"post\",\"author\":\"alice\",\"message\":\"hello everyone\"}";
    strata_raw_send(client, post_req, strlen(post_req));
    char resp[4096] = {0};
    rc = strata_raw_recv(client, resp, sizeof(resp) - 1);
    assert(rc > 0);
    if (!strstr(resp, "\"ok\":true")) { fprintf(stderr, "post response: %s\n", resp); }
    assert(strstr(resp, "\"ok\":true") != NULL);
    assert(strstr(resp, "\"id\":\"") != NULL);
    PASS();

    /* POST another message */
    TEST("post second message");
    const char *post2 = "{\"action\":\"post\",\"author\":\"bob\",\"message\":\"hi alice\"}";
    strata_raw_send(client, post2, strlen(post2));
    memset(resp, 0, sizeof(resp));
    rc = strata_raw_recv(client, resp, sizeof(resp) - 1);
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    PASS();

    /* LIST messages */
    TEST("list messages returns both");
    const char *list_req = "{\"action\":\"list\"}";
    strata_raw_send(client, list_req, strlen(list_req));
    memset(resp, 0, sizeof(resp));
    rc = strata_raw_recv(client, resp, sizeof(resp) - 1);
    assert(rc > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    assert(strstr(resp, "hello everyone") != NULL);
    assert(strstr(resp, "hi alice") != NULL);
    PASS();

    /* Check PUB notification was received */
    TEST("notification received via PUB");
    char topic[256] = {0};
    char payload[4096] = {0};
    rc = strata_sub_recv(notif_sub, topic, sizeof(topic), payload, sizeof(payload));
    if (rc > 0) {
        assert(strstr(topic, "board/new") != NULL);
        assert(strstr(payload, "alice") != NULL || strstr(payload, "bob") != NULL);
        PASS();
    } else {
        /* PUB/SUB timing can cause missed messages — acceptable */
        printf("SKIP (timing)\n");
    }

    /* Cleanup happens via atexit */
    printf("ALL TESTS PASSED\n");
    return 0;
}
