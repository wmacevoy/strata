/*
 * Integration check: shamir-bf links and works inside the strata build.
 *
 * The algorithm itself — both field backends, the object model, primality,
 * the brainwallet vectors — is tested in shamir-bf's own suite. This only
 * proves the dependency is wired up and usable from here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "shamir.h"

int main(void) {
    printf("test_shamir (shamir-bf integration)\n");

    /* Binary field: byte-exact, for future vouch shares. */
    printf("  %-50s", "GF(2^128) split 3-of-5, recover 2,4,5");
    fflush(stdout);
    GF2Field *g = GF2Field_new(16);
    assert(g);
    ShamirField *f = (ShamirField *)g;          /* upcast: a plain cast */
    char secret[64];
    assert(ShamirField_random(f, secret, sizeof secret) == SHAMIR_OK);
    char *keys[5];
    assert(ShamirField_split(f, secret, 3, 5, keys) == SHAMIR_OK);
    int idx[] = {2, 4, 5};
    const char *kh[] = {keys[1], keys[3], keys[4]};
    char out[64];
    assert(ShamirField_recover(f, idx, kh, 3, 0, out, sizeof out) == SHAMIR_OK);
    assert(strcmp(out, secret) == 0);
    for (int i = 0; i < 5; i++) free(keys[i]);
    printf("PASS\n");

    /* The backend is identified by a checked downcast, not a flag. */
    printf("  %-50s", "GF2Field_cast identifies it; PrimeField_cast refuses");
    fflush(stdout);
    assert(GF2Field_cast(f) == g);
    assert(PrimeField_cast(f) == NULL);
    assert(strcmp(GF2Field_polyHex(g), "00000000000000000000000000000087") == 0);
    GF2Field_delete(g);
    printf("PASS\n");

    /* Prime field: brainwallet interop, through the same base-class calls. */
    printf("  %-50s", "brainwallet vector recovers deadbeef42");
    fflush(stdout);
    PrimeField *p = PrimeField_new(SHAMIR_BRAINWALLET_PRIME);
    assert(p);
    int pidx[] = {1, 3};
    const char *pkeys[] = {"123457573e6abd31", "369d04485fc2590f"};
    assert(ShamirField_recover((ShamirField *)p, pidx, pkeys, 2, 0,
                               out, sizeof out) == SHAMIR_OK);
    assert(strcmp(out, "deadbeef42") == 0);
    PrimeField_delete(p);
    printf("PASS\n");

    /* Prime generation, for choosing a field width. */
    printf("  %-50s", "primeForBytes(16) == 2^128 - 159");
    fflush(stdout);
    char prime[64];
    assert(PrimeField_primeForBytes(16, prime, sizeof prime) == SHAMIR_OK);
    assert(strcmp(prime, "ffffffffffffffffffffffffffffff61") == 0);
    printf("PASS\n");

    printf("ALL TESTS PASSED\n");
    return 0;
}
