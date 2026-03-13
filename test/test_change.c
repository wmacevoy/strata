#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "strata/msg.h"
#include "strata/store.h"
#include "strata/context.h"
#include "strata/change.h"

/* From store.c */
extern void strata_store_attach_change_pub(strata_store *store, strata_change_pub *pub);

#define TEST(name) printf("  %-50s", name)
#define PASS()     printf("PASS\n")

int main(void) {
    const char *db_path = "/tmp/strata_test_change.db";
    const char *endpoint = "tcp://127.0.0.1:15555";
    unlink(db_path);

    printf("test_change\n");

    /* Set up store */
    strata_store *store = strata_store_open_sqlite(db_path);
    assert(store != NULL);
    assert(strata_store_init(store) == 0);
    assert(strata_repo_create(store, "proj-1", "Project One") == 0);
    assert(strata_role_assign(store, "alice", "developer", "proj-1") == 0);

    /* Set up publisher */
    TEST("create change publisher");
    strata_change_pub *pub = strata_change_pub_create(endpoint);
    assert(pub != NULL);
    strata_store_attach_change_pub(store, pub);
    PASS();

    /* Set up subscriber */
    TEST("create subscriber");
    strata_sock *sub = strata_sub_connect(endpoint);
    assert(sub != NULL);
    strata_sub_subscribe(sub, "change/");
    PASS();

    /* Give time to establish connection */
    usleep(100000);  /* 100ms */

    /* Put an artifact — should trigger change event */
    TEST("put artifact triggers change event");
    strata_ctx *alice = strata_ctx_create("alice");
    const char *data = "hello world";
    const char *roles[] = {"developer"};
    char artifact_id[65];
    assert(strata_artifact_put(store, alice, "proj-1", "file",
           data, strlen(data), roles, 1, artifact_id) == 0);

    /* Receive the change event */
    char topic[512] = {0};
    char payload[1024] = {0};

    /* Set a receive timeout so we don't hang forever */
    strata_msg_set_timeout(sub, 2000, -1);

    int rc = strata_sub_recv(sub, topic, sizeof(topic), payload, sizeof(payload));
    assert(rc > 0);
    PASS();

    TEST("topic is change/proj-1/file");
    assert(strcmp(topic, "change/proj-1/file") == 0);
    PASS();

    TEST("payload contains artifact_id");
    assert(strstr(payload, artifact_id) != NULL);
    PASS();

    TEST("payload contains change type");
    assert(strstr(payload, "\"change\":\"create\"") != NULL);
    PASS();

    /* Cleanup */
    strata_ctx_free(alice);
    strata_sock_close(sub);
    strata_store_close(store);  /* also frees change_pub */
    unlink(db_path);

    printf("ALL TESTS PASSED\n");
    return 0;
}
