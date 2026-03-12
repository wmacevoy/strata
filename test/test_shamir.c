/*
 * Test Shamir Secret Sharing implementation.
 *
 * Verifies:
 *   1. split + recover round-trip (2-of-3, 3-of-5)
 *   2. any M shares recover the secret (not just the first M)
 *   3. fewer than M shares do NOT recover (wrong value)
 *   4. random secret generation
 *   5. compatibility with brainwallet default prime (2^132 - 347)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "strata/shamir.h"

#define TEST(name) printf("  %-50s", name); fflush(stdout)
#define PASS()     printf("PASS\n")

static void test_split_recover_2of3(void) {
    TEST("split 2-of-3, recover from shares 1,2");

    const char *secret = "deadbeef42";
    char *keys[3];

    int rc = strata_shamir_split(secret, 2, 3, NULL, keys);
    assert(rc == 0);

    /* recover from shares 1 and 2 */
    int indices[] = {1, 2};
    const char *kh[] = {keys[0], keys[1]};
    char recovered[128];
    rc = strata_shamir_recover(indices, kh, 2, 0, NULL, recovered, sizeof(recovered));
    assert(rc == 0);
    assert(strcmp(recovered, secret) == 0);

    for (int i = 0; i < 3; i++) free(keys[i]);
    PASS();
}

static void test_any_2_of_3(void) {
    TEST("any 2 of 3 shares recover secret");

    const char *secret = "abc123def456";
    char *keys[3];

    int rc = strata_shamir_split(secret, 2, 3, NULL, keys);
    assert(rc == 0);

    /* try all 3 pairs: (1,2), (1,3), (2,3) */
    int pairs[][2] = {{1,2}, {1,3}, {2,3}};
    for (int p = 0; p < 3; p++) {
        int idx[] = {pairs[p][0], pairs[p][1]};
        const char *kh[] = {keys[idx[0]-1], keys[idx[1]-1]};
        char recovered[128];
        rc = strata_shamir_recover(idx, kh, 2, 0, NULL, recovered, sizeof(recovered));
        assert(rc == 0);
        assert(strcmp(recovered, secret) == 0);
    }

    for (int i = 0; i < 3; i++) free(keys[i]);
    PASS();
}

static void test_3of5(void) {
    TEST("split 3-of-5, recover from shares 2,4,5");

    const char *secret = "1234567890abcdef";
    char *keys[5];

    int rc = strata_shamir_split(secret, 3, 5, NULL, keys);
    assert(rc == 0);

    int indices[] = {2, 4, 5};
    const char *kh[] = {keys[1], keys[3], keys[4]};
    char recovered[128];
    rc = strata_shamir_recover(indices, kh, 3, 0, NULL, recovered, sizeof(recovered));
    assert(rc == 0);
    assert(strcmp(recovered, secret) == 0);

    for (int i = 0; i < 5; i++) free(keys[i]);
    PASS();
}

static void test_insufficient_shares(void) {
    TEST("1 of 3 shares gives wrong value (M=2)");

    const char *secret = "deadbeef42";
    char *keys[3];

    int rc = strata_shamir_split(secret, 2, 3, NULL, keys);
    assert(rc == 0);

    /* try recovering with only 1 share — should give wrong answer */
    int indices[] = {1};
    const char *kh[] = {keys[0]};
    char recovered[128];
    rc = strata_shamir_recover(indices, kh, 1, 0, NULL, recovered, sizeof(recovered));
    assert(rc == 0);  /* no error, just wrong value */
    assert(strcmp(recovered, secret) != 0);

    for (int i = 0; i < 3; i++) free(keys[i]);
    PASS();
}

static void test_random_secret(void) {
    TEST("random secret generation");

    char hex1[128], hex2[128];
    int rc = strata_shamir_random(NULL, hex1, sizeof(hex1));
    assert(rc == 0);
    assert(strlen(hex1) > 0);

    rc = strata_shamir_random(NULL, hex2, sizeof(hex2));
    assert(rc == 0);
    assert(strlen(hex2) > 0);

    /* two randoms should differ (astronomically unlikely to match) */
    assert(strcmp(hex1, hex2) != 0);

    PASS();
}

static void test_random_split_recover(void) {
    TEST("random secret → split 3-of-5 → recover");

    char secret[128];
    int rc = strata_shamir_random(NULL, secret, sizeof(secret));
    assert(rc == 0);

    char *keys[5];
    rc = strata_shamir_split(secret, 3, 5, NULL, keys);
    assert(rc == 0);

    /* recover from shares 1, 3, 5 */
    int indices[] = {1, 3, 5};
    const char *kh[] = {keys[0], keys[2], keys[4]};
    char recovered[128];
    rc = strata_shamir_recover(indices, kh, 3, 0, NULL, recovered, sizeof(recovered));
    assert(rc == 0);
    assert(strcmp(recovered, secret) == 0);

    for (int i = 0; i < 5; i++) free(keys[i]);
    PASS();
}

static void test_custom_prime(void) {
    TEST("custom prime (2^96 - 17)");

    /* 2^96 - 17 = 79228162514264337593543950319 = 0xFFFFFFFFFFFFFFFFFFFFFFEF */
    const char *prime = "ffffffffffffffffffffffef";
    const char *secret = "42";
    char *keys[3];

    int rc = strata_shamir_split(secret, 2, 3, prime, keys);
    assert(rc == 0);

    int indices[] = {2, 3};
    const char *kh[] = {keys[1], keys[2]};
    char recovered[128];
    rc = strata_shamir_recover(indices, kh, 2, 0, prime, recovered, sizeof(recovered));
    assert(rc == 0);
    assert(strcmp(recovered, "42") == 0);

    for (int i = 0; i < 3; i++) free(keys[i]);
    PASS();
}

static void test_recover_key(void) {
    TEST("recover key at index 3 from shares 1,2");

    const char *secret = "beef";
    char *keys[3];

    int rc = strata_shamir_split(secret, 2, 3, NULL, keys);
    assert(rc == 0);

    /* recover key 3 from shares 1 and 2 */
    int indices[] = {1, 2};
    const char *kh[] = {keys[0], keys[1]};
    char recovered[128];
    rc = strata_shamir_recover(indices, kh, 2, 3, NULL, recovered, sizeof(recovered));
    assert(rc == 0);
    assert(strcmp(recovered, keys[2]) == 0);

    for (int i = 0; i < 3; i++) free(keys[i]);
    PASS();
}

static void test_brainwallet_compat(void) {
    TEST("brainwallet cross-compatibility");

    /*
     * Known test vector from Python brainwallet:
     *   prime = 2^132 - 347
     *   polynomial: f(x) = 0xdeadbeef42 + 0x1234567890abcdef * x  (mod prime)
     *   f(1) = 0x123457573e6abd31
     *   f(2) = 0x2468adcfcf168b20
     *   f(3) = 0x369d04485fc2590f
     *   Lagrange recover(f(1), f(3)) at x=0 → 0xdeadbeef42
     */
    int indices[] = {1, 3};
    const char *keys[] = {"123457573e6abd31", "369d04485fc2590f"};
    char recovered[128];

    int rc = strata_shamir_recover(indices, keys, 2, 0, NULL,
                                   recovered, sizeof(recovered));
    assert(rc == 0);
    assert(strcmp(recovered, "deadbeef42") == 0);

    PASS();
}

int main(void) {
    printf("test_shamir\n");

    test_split_recover_2of3();
    test_any_2_of_3();
    test_3of5();
    test_insufficient_shares();
    test_random_secret();
    test_random_split_recover();
    test_custom_prime();
    test_recover_key();
    test_brainwallet_compat();

    printf("ALL TESTS PASSED\n");
    return 0;
}
