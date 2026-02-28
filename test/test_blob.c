#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "strata/store.h"
#include "strata/context.h"
#include "strata/blob.h"

#define TEST(name) printf("  %-55s", name)
#define PASS()     printf("PASS\n")

static int find_count;
static char found_ids[16][65];

static int find_cb(const strata_blob *b, void *userdata) {
    (void)userdata;
    if (find_count < 16)
        memcpy(found_ids[find_count], b->blob_id, 65);
    find_count++;
    return 1;
}

int main(void) {
    const char *db_path = "/tmp/strata_test_blob.db";
    unlink(db_path);

    printf("test_blob\n");

    strata_store *store = strata_store_open_sqlite(db_path);
    assert(store != NULL);
    strata_store_init(store);

    /* Create global repo for blob role assignments */
    strata_repo_create(store, "global", "Global");

    /* Need role_assignments table for permission checks */
    strata_role_assign(store, "alice", "engineer", "global");
    strata_role_assign(store, "alice", "manager", "global");
    strata_role_assign(store, "bob", "engineer", "global");
    strata_role_assign(store, "eve", "intern", "global");

    strata_ctx *alice = strata_ctx_create("alice");
    strata_ctx *bob = strata_ctx_create("bob");
    strata_ctx *eve = strata_ctx_create("eve");

    char id1[65], id2[65], id3[65];

    /* Put blobs with different tags and permissions */
    TEST("put blob with tags [project:alpha, type:doc] roles [engineer]");
    const char *tags1[] = {"project:alpha", "type:doc"};
    const char *roles1[] = {"engineer"};
    assert(strata_blob_put(store, alice, "design document v1", 18,
           tags1, 2, roles1, 1, id1) == 0);
    PASS();

    TEST("put blob with tags [project:alpha, type:code] roles [engineer]");
    const char *tags2[] = {"project:alpha", "type:code"};
    const char *roles2[] = {"engineer"};
    assert(strata_blob_put(store, alice, "int main() { return 0; }", 24,
           tags2, 2, roles2, 1, id2) == 0);
    PASS();

    TEST("put blob with tags [project:beta, type:doc] roles [manager]");
    const char *tags3[] = {"project:beta", "type:doc"};
    const char *roles3[] = {"manager"};
    assert(strata_blob_put(store, alice, "confidential roadmap", 20,
           tags3, 2, roles3, 1, id3) == 0);
    PASS();

    /* Get by ID — permission filtered */
    TEST("alice (engineer+manager) can get all three");
    strata_blob out;
    assert(strata_blob_get(store, alice, id1, &out) == 0);
    assert(out.content_len == 18);
    strata_blob_cleanup(&out);
    assert(strata_blob_get(store, alice, id2, &out) == 0);
    strata_blob_cleanup(&out);
    assert(strata_blob_get(store, alice, id3, &out) == 0);
    strata_blob_cleanup(&out);
    PASS();

    TEST("bob (engineer) can get id1, id2 but NOT id3 (manager only)");
    assert(strata_blob_get(store, bob, id1, &out) == 0);
    strata_blob_cleanup(&out);
    assert(strata_blob_get(store, bob, id2, &out) == 0);
    strata_blob_cleanup(&out);
    assert(strata_blob_get(store, bob, id3, &out) == -1);
    PASS();

    TEST("eve (intern) cannot get any blob");
    assert(strata_blob_get(store, eve, id1, &out) == -1);
    assert(strata_blob_get(store, eve, id2, &out) == -1);
    assert(strata_blob_get(store, eve, id3, &out) == -1);
    PASS();

    /* Find by tags */
    TEST("find [project:alpha] → 2 blobs for alice");
    find_count = 0;
    const char *ftags1[] = {"project:alpha"};
    strata_blob_find(store, alice, ftags1, 1, find_cb, NULL);
    assert(find_count == 2);
    PASS();

    TEST("find [project:alpha, type:code] → 1 blob (AND logic)");
    find_count = 0;
    const char *ftags2[] = {"project:alpha", "type:code"};
    strata_blob_find(store, alice, ftags2, 2, find_cb, NULL);
    assert(find_count == 1);
    PASS();

    TEST("find [type:doc] → 2 for alice, 1 for bob");
    find_count = 0;
    const char *ftags3[] = {"type:doc"};
    strata_blob_find(store, alice, ftags3, 1, find_cb, NULL);
    assert(find_count == 2);
    find_count = 0;
    strata_blob_find(store, bob, ftags3, 1, find_cb, NULL);
    assert(find_count == 1);  /* bob can't see manager-only blob */
    PASS();

    TEST("find [] (all) → 3 for alice, 2 for bob, 0 for eve");
    find_count = 0;
    strata_blob_find(store, alice, NULL, 0, find_cb, NULL);
    assert(find_count == 3);
    find_count = 0;
    strata_blob_find(store, bob, NULL, 0, find_cb, NULL);
    assert(find_count == 2);
    find_count = 0;
    strata_blob_find(store, eve, NULL, 0, find_cb, NULL);
    assert(find_count == 0);
    PASS();

    /* Tag management */
    TEST("add tag 'priority:high' to blob1");
    assert(strata_blob_tag(store, id1, "priority:high") == 0);
    PASS();

    TEST("find [priority:high] → 1 blob");
    find_count = 0;
    const char *ftags4[] = {"priority:high"};
    strata_blob_find(store, alice, ftags4, 1, find_cb, NULL);
    assert(find_count == 1);
    PASS();

    TEST("list tags on blob1 → 3 tags");
    char **btags; int btag_count;
    strata_blob_tags(store, id1, &btags, &btag_count);
    assert(btag_count == 3);
    strata_blob_free_tags(btags, btag_count);
    PASS();

    TEST("untag 'priority:high', find returns 0");
    assert(strata_blob_untag(store, id1, "priority:high") == 0);
    find_count = 0;
    strata_blob_find(store, alice, ftags4, 1, find_cb, NULL);
    assert(find_count == 0);
    PASS();

    /* Find with non-existent tag */
    TEST("find [nonexistent] → 0");
    find_count = 0;
    const char *ftags5[] = {"nonexistent"};
    strata_blob_find(store, alice, ftags5, 1, find_cb, NULL);
    assert(find_count == 0);
    PASS();

    strata_ctx_free(alice);
    strata_ctx_free(bob);
    strata_ctx_free(eve);
    strata_store_close(store);
    unlink(db_path);

    printf("ALL TESTS PASSED\n");
    return 0;
}
