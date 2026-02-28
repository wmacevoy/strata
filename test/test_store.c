#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "strata/store.h"
#include "strata/context.h"

#define TEST(name) printf("  %-50s", name)
#define PASS()     printf("PASS\n")

static int list_count;
static int list_cb(const strata_artifact *a, void *userdata) {
    (void)userdata;
    list_count++;
    return 1;
}

static char last_listed_id[65];
static int list_capture_cb(const strata_artifact *a, void *userdata) {
    (void)userdata;
    memcpy(last_listed_id, a->artifact_id, 65);
    list_count++;
    return 1;
}

int main(void) {
    const char *db_path = "/tmp/strata_test.db";
    unlink(db_path);

    printf("test_store\n");

    /* Open and init */
    TEST("open sqlite store");
    strata_store *store = strata_store_open_sqlite(db_path);
    assert(store != NULL);
    PASS();

    TEST("init schema");
    assert(strata_store_init(store) == 0);
    PASS();

    /* Create repo */
    TEST("create repo");
    assert(strata_repo_create(store, "proj-1", "Project One") == 0);
    PASS();

    /* Assign roles */
    TEST("assign roles");
    assert(strata_role_assign(store, "alice", "developer", "proj-1") == 0);
    assert(strata_role_assign(store, "alice", "auditor", "proj-1") == 0);
    assert(strata_role_assign(store, "bob", "developer", "proj-1") == 0);
    assert(strata_role_assign(store, "eve", "intern", "proj-1") == 0);
    PASS();

    /* Create contexts */
    strata_ctx *alice = strata_ctx_create("alice");
    strata_ctx *bob = strata_ctx_create("bob");
    strata_ctx *eve = strata_ctx_create("eve");

    /* Put artifacts with different role access */
    char id_public[65], id_dev[65], id_audit[65];

    TEST("put artifact visible to developer+auditor");
    const char *data1 = "public code file";
    const char *roles1[] = {"developer", "auditor"};
    assert(strata_artifact_put(store, alice, "proj-1", "file",
           data1, strlen(data1), roles1, 2, id_public) == 0);
    PASS();

    TEST("put artifact visible to developer only");
    const char *data2 = "developer secret";
    const char *roles2[] = {"developer"};
    assert(strata_artifact_put(store, alice, "proj-1", "file",
           data2, strlen(data2), roles2, 1, id_dev) == 0);
    PASS();

    TEST("put artifact visible to auditor only");
    const char *data3 = "audit findings";
    const char *roles3[] = {"auditor"};
    assert(strata_artifact_put(store, alice, "proj-1", "wiki",
           data3, strlen(data3), roles3, 1, id_audit) == 0);
    PASS();

    /* Test role-filtered GET */
    strata_artifact out;

    TEST("alice (developer+auditor) can read all three");
    assert(strata_artifact_get(store, alice, id_public, &out) == 0);
    strata_artifact_cleanup(&out);
    assert(strata_artifact_get(store, alice, id_dev, &out) == 0);
    strata_artifact_cleanup(&out);
    assert(strata_artifact_get(store, alice, id_audit, &out) == 0);
    strata_artifact_cleanup(&out);
    PASS();

    TEST("bob (developer) can read public + dev, NOT audit");
    assert(strata_artifact_get(store, bob, id_public, &out) == 0);
    strata_artifact_cleanup(&out);
    assert(strata_artifact_get(store, bob, id_dev, &out) == 0);
    strata_artifact_cleanup(&out);
    assert(strata_artifact_get(store, bob, id_audit, &out) == -1);
    PASS();

    TEST("eve (intern) cannot read any of the three");
    assert(strata_artifact_get(store, eve, id_public, &out) == -1);
    assert(strata_artifact_get(store, eve, id_dev, &out) == -1);
    assert(strata_artifact_get(store, eve, id_audit, &out) == -1);
    PASS();

    /* Test role-filtered LIST */
    TEST("alice lists all 3 artifacts");
    list_count = 0;
    strata_artifact_list(store, alice, "proj-1", NULL, list_cb, NULL);
    assert(list_count == 3);
    PASS();

    TEST("bob lists 2 artifacts (developer only)");
    list_count = 0;
    strata_artifact_list(store, bob, "proj-1", NULL, list_cb, NULL);
    assert(list_count == 2);
    PASS();

    TEST("eve lists 0 artifacts");
    list_count = 0;
    strata_artifact_list(store, eve, "proj-1", NULL, list_cb, NULL);
    assert(list_count == 0);
    PASS();

    /* Test type filtering */
    TEST("alice lists only wiki artifacts (1)");
    list_count = 0;
    strata_artifact_list(store, alice, "proj-1", "wiki", list_cb, NULL);
    assert(list_count == 1);
    PASS();

    /* Test role revocation */
    TEST("revoke bob's developer role, now sees 0");
    assert(strata_role_revoke(store, "bob", "developer", "proj-1") == 0);
    list_count = 0;
    strata_artifact_list(store, bob, "proj-1", NULL, list_cb, NULL);
    assert(list_count == 0);
    assert(strata_artifact_get(store, bob, id_public, &out) == -1);
    PASS();

    /* Cleanup */
    strata_ctx_free(alice);
    strata_ctx_free(bob);
    strata_ctx_free(eve);
    strata_store_close(store);
    unlink(db_path);

    printf("ALL TESTS PASSED\n");
    return 0;
}
