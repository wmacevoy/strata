/*
 * Shamir Secret Sharing — C implementation using libbf + libsodium.
 *
 * Core algorithm from brainwallet/shamir.py:
 *   - Polynomial evaluation mod prime for share generation
 *   - Lagrange interpolation mod prime for recovery
 *   - Extended GCD for modular inverse
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sodium.h>
#include "libbf.h"
#include "strata/shamir.h"

/* --- libbf context --- */

static void *bf_realloc_wrapper(void *opaque, void *ptr, size_t size) {
    (void)opaque;
    if (size == 0) { free(ptr); return NULL; }
    return realloc(ptr, size);
}

static bf_context_t *get_bf_ctx(void) {
    static bf_context_t ctx;
    static int inited = 0;
    if (!inited) {
        bf_context_init(&ctx, bf_realloc_wrapper, NULL);
        inited = 1;
    }
    return &ctx;
}

/* --- helpers --- */

/* Parse hex string to bf_t. Returns 0 on success. */
static int hex_to_bf(bf_t *r, const char *hex) {
    if (!hex || !hex[0]) return -1;
    /* skip optional 0x prefix */
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
    bf_atof(r, hex, NULL, 16, BF_PREC_INF, BF_RNDZ);
    return 0;
}

/* Convert bf_t to hex string into caller buffer. Returns 0 on success. */
static int bf_to_hex(const bf_t *a, char *out, size_t out_size) {
    size_t len;
    char *s = bf_ftoa(&len, a, 16, BF_PREC_INF, BF_RNDZ | BF_FTOA_FORMAT_FREE);
    if (!s) return -1;
    if (len + 1 > out_size) { free(s); return -1; }
    memcpy(out, s, len + 1);
    free(s);
    return 0;
}

/* Convert bf_t to malloc'd hex string. */
static char *bf_to_hex_alloc(const bf_t *a) {
    size_t len;
    char *s = bf_ftoa(&len, a, 16, BF_PREC_INF, BF_RNDZ | BF_FTOA_FORMAT_FREE);
    return s;
}

/* r = a mod p (Euclidean, result always >= 0). Handles r aliasing a or p. */
static void bf_modp(bf_t *r, const bf_t *a, const bf_t *p) {
    bf_context_t *ctx = get_bf_ctx();
    bf_t q, rem;
    bf_init(ctx, &q);
    bf_init(ctx, &rem);
    bf_divrem(&q, &rem, a, p, BF_PREC_INF, BF_RNDZ, BF_DIVREM_EUCLIDIAN);
    bf_set(r, &rem);
    bf_delete(&q);
    bf_delete(&rem);
}

/* r = (a * b) mod p */
static void bf_mulmod(bf_t *r, const bf_t *a, const bf_t *b, const bf_t *p) {
    bf_t tmp;
    bf_init(get_bf_ctx(), &tmp);
    bf_mul(&tmp, a, b, BF_PREC_INF, BF_RNDZ);
    bf_modp(r, &tmp, p);
    bf_delete(&tmp);
}

/* r = (a + b) mod p */
static void bf_addmod(bf_t *r, const bf_t *a, const bf_t *b, const bf_t *p) {
    bf_t tmp;
    bf_init(get_bf_ctx(), &tmp);
    bf_add(&tmp, a, b, BF_PREC_INF, BF_RNDZ);
    bf_modp(r, &tmp, p);
    bf_delete(&tmp);
}

/* r = (a - b) mod p */
static void bf_submod(bf_t *r, const bf_t *a, const bf_t *b, const bf_t *p) {
    bf_t tmp;
    bf_init(get_bf_ctx(), &tmp);
    bf_sub(&tmp, a, b, BF_PREC_INF, BF_RNDZ);
    bf_modp(r, &tmp, p);
    bf_delete(&tmp);
}

/*
 * Extended GCD: returns (x, y) such that a*x + b*y = gcd(a, b).
 * We only need x (the modular inverse of a mod b when gcd=1).
 */
static void extended_gcd(bf_t *x_out, const bf_t *a, const bf_t *b) {
    bf_context_t *ctx = get_bf_ctx();
    bf_t old_r, r, old_s, s, q, tmp, tmp2;

    bf_init(ctx, &old_r); bf_set(&old_r, a);
    bf_init(ctx, &r);     bf_set(&r, b);
    bf_init(ctx, &old_s);  bf_set_si(&old_s, 1);
    bf_init(ctx, &s);      bf_set_si(&s, 0);
    bf_init(ctx, &q);
    bf_init(ctx, &tmp);
    bf_init(ctx, &tmp2);

    while (!bf_is_zero(&r)) {
        /* q = old_r / r (integer division) */
        bf_divrem(&q, &tmp, &old_r, &r, BF_PREC_INF, BF_RNDZ, BF_DIVREM_EUCLIDIAN);

        /* (old_r, r) = (r, old_r - q*r) = (r, tmp) */
        bf_set(&old_r, &r);
        bf_set(&r, &tmp);

        /* (old_s, s) = (s, old_s - q*s) */
        bf_mul(&tmp2, &q, &s, BF_PREC_INF, BF_RNDZ);
        bf_sub(&tmp, &old_s, &tmp2, BF_PREC_INF, BF_RNDZ);
        bf_set(&old_s, &s);
        bf_set(&s, &tmp);
    }

    bf_set(x_out, &old_s);

    bf_delete(&old_r); bf_delete(&r);
    bf_delete(&old_s); bf_delete(&s);
    bf_delete(&q); bf_delete(&tmp); bf_delete(&tmp2);
}

/* r = (num / den) mod p  — modular division via extended GCD */
static void bf_divmod(bf_t *r, const bf_t *num, const bf_t *den, const bf_t *p) {
    bf_context_t *ctx = get_bf_ctx();
    bf_t inv, n, d;

    bf_init(ctx, &inv);
    bf_init(ctx, &n);
    bf_init(ctx, &d);

    bf_modp(&n, num, p);
    bf_modp(&d, den, p);

    extended_gcd(&inv, &d, p);
    bf_modp(&inv, &inv, p);  /* ensure positive */

    bf_mulmod(r, &n, &inv, p);

    bf_delete(&inv); bf_delete(&n); bf_delete(&d);
}

/* Evaluate polynomial cs[0..degree-1] at x, mod p (Horner's method) */
static void eval_poly(bf_t *r, const bf_t *cs, int degree, const bf_t *x, const bf_t *p) {
    bf_context_t *ctx = get_bf_ctx();
    bf_set_si(r, 0);
    for (int i = degree - 1; i >= 0; i--) {
        bf_mulmod(r, r, x, p);
        bf_addmod(r, r, &cs[i], p);
    }
    (void)ctx;
}

/* Generate random bf_t in [0, p-1] using libsodium */
static void random_below(bf_t *r, const bf_t *p) {
    if (sodium_init() < 0) return;

    /* determine byte count from prime */
    char *phex = bf_to_hex_alloc(p);
    if (!phex) return;
    size_t hex_len = strlen(phex);
    size_t byte_count = (hex_len + 1) / 2;
    free(phex);

    bf_context_t *ctx = get_bf_ctx();
    uint8_t *buf = malloc(byte_count);
    if (!buf) return;

    /* rejection sampling */
    for (;;) {
        randombytes_buf(buf, byte_count);

        /* convert bytes to hex string */
        char *hex = malloc(byte_count * 2 + 1);
        if (!hex) { free(buf); return; }
        for (size_t i = 0; i < byte_count; i++)
            sprintf(hex + i * 2, "%02x", buf[i]);
        hex[byte_count * 2] = '\0';

        hex_to_bf(r, hex);
        free(hex);

        if (bf_cmp_lt(r, p)) break;  /* in range */
    }

    free(buf);
    (void)ctx;
}

/* --- Public API --- */

int strata_shamir_split(const char *secret_hex, int minimum, int shares,
                        const char *prime_hex, char **out_keys) {
    if (!secret_hex || minimum < 1 || shares < minimum || !out_keys)
        return -1;

    bf_context_t *ctx = get_bf_ctx();
    bf_t prime, secret;

    bf_init(ctx, &prime);
    bf_init(ctx, &secret);

    /* parse prime */
    if (!prime_hex) prime_hex = STRATA_SHAMIR_DEFAULT_PRIME;
    if (hex_to_bf(&prime, prime_hex) != 0) goto fail;

    /* parse secret */
    if (hex_to_bf(&secret, secret_hex) != 0) goto fail;
    if (!bf_cmp_lt(&secret, &prime)) goto fail;  /* secret must be < prime */

    /* build polynomial: cs[0] = secret, cs[1..minimum-1] = random */
    bf_t *cs = malloc(sizeof(bf_t) * minimum);
    if (!cs) goto fail;
    for (int i = 0; i < minimum; i++)
        bf_init(ctx, &cs[i]);

    bf_set(&cs[0], &secret);
    for (int i = 1; i < minimum; i++)
        random_below(&cs[i], &prime);

    /* evaluate polynomial at x = 1..shares */
    bf_t x, val;
    bf_init(ctx, &x);
    bf_init(ctx, &val);

    for (int i = 0; i < shares; i++) {
        bf_set_si(&x, i + 1);
        eval_poly(&val, cs, minimum, &x, &prime);
        out_keys[i] = bf_to_hex_alloc(&val);
        if (!out_keys[i]) {
            /* cleanup on failure */
            for (int j = 0; j < i; j++) free(out_keys[j]);
            bf_delete(&x); bf_delete(&val);
            for (int j = 0; j < minimum; j++) bf_delete(&cs[j]);
            free(cs);
            goto fail;
        }
    }

    bf_delete(&x); bf_delete(&val);
    for (int i = 0; i < minimum; i++) bf_delete(&cs[i]);
    free(cs);
    bf_delete(&prime); bf_delete(&secret);
    return 0;

fail:
    bf_delete(&prime); bf_delete(&secret);
    return -1;
}

int strata_shamir_recover(const int *indices, const char **keys_hex, int count,
                          int target, const char *prime_hex,
                          char *out_hex, size_t out_size) {
    if (!indices || !keys_hex || count < 1 || !out_hex || out_size < 3)
        return -1;

    bf_context_t *ctx = get_bf_ctx();
    bf_t prime;
    bf_init(ctx, &prime);

    if (!prime_hex) prime_hex = STRATA_SHAMIR_DEFAULT_PRIME;
    if (hex_to_bf(&prime, prime_hex) != 0) { bf_delete(&prime); return -1; }

    /* parse keys and indices into bf_t arrays */
    bf_t *xs = malloc(sizeof(bf_t) * count);
    bf_t *ys = malloc(sizeof(bf_t) * count);
    if (!xs || !ys) { free(xs); free(ys); bf_delete(&prime); return -1; }

    for (int i = 0; i < count; i++) {
        bf_init(ctx, &xs[i]);
        bf_init(ctx, &ys[i]);
        bf_set_si(&xs[i], indices[i]);
        if (hex_to_bf(&ys[i], keys_hex[i]) != 0) {
            for (int j = 0; j <= i; j++) { bf_delete(&xs[j]); bf_delete(&ys[j]); }
            free(xs); free(ys); bf_delete(&prime);
            return -1;
        }
    }

    /*
     * Lagrange interpolation at x = target.
     * Matches brainwallet/shamir.py _lagrangeInterpolate exactly.
     */
    bf_t xt, result;
    bf_init(ctx, &xt);
    bf_init(ctx, &result);
    bf_set_si(&xt, target);

    /* compute nums[i] = product(xt - xs[j]) for j != i
     * compute dens[i] = product(xs[i] - xs[j]) for j != i
     */
    bf_t *nums = malloc(sizeof(bf_t) * count);
    bf_t *dens = malloc(sizeof(bf_t) * count);
    if (!nums || !dens) goto cleanup;

    for (int i = 0; i < count; i++) {
        bf_init(ctx, &nums[i]);
        bf_init(ctx, &dens[i]);
        bf_set_si(&nums[i], 1);
        bf_set_si(&dens[i], 1);

        for (int j = 0; j < count; j++) {
            if (j == i) continue;
            bf_t diff;
            bf_init(ctx, &diff);

            /* nums[i] *= (xt - xs[j]) mod p */
            bf_submod(&diff, &xt, &xs[j], &prime);
            bf_mulmod(&nums[i], &nums[i], &diff, &prime);

            /* dens[i] *= (xs[i] - xs[j]) mod p */
            bf_submod(&diff, &xs[i], &xs[j], &prime);
            bf_mulmod(&dens[i], &dens[i], &diff, &prime);

            bf_delete(&diff);
        }
    }

    /* den = product of all dens[i] */
    bf_t den;
    bf_init(ctx, &den);
    bf_set_si(&den, 1);
    for (int i = 0; i < count; i++)
        bf_mulmod(&den, &den, &dens[i], &prime);

    /* num = sum of (nums[i] * den * ys[i] / dens[i]) mod p */
    bf_set_si(&result, 0);
    for (int i = 0; i < count; i++) {
        bf_t term, tmp;
        bf_init(ctx, &term);
        bf_init(ctx, &tmp);

        bf_mulmod(&term, &nums[i], &den, &prime);
        bf_mulmod(&term, &term, &ys[i], &prime);
        bf_divmod(&tmp, &term, &dens[i], &prime);
        bf_addmod(&result, &result, &tmp, &prime);

        bf_delete(&term); bf_delete(&tmp);
    }

    /* result = num / den mod p */
    bf_t final;
    bf_init(ctx, &final);
    bf_divmod(&final, &result, &den, &prime);

    int rc = bf_to_hex(&final, out_hex, out_size);
    bf_delete(&final);

    bf_delete(&den);
    for (int i = 0; i < count; i++) {
        bf_delete(&nums[i]); bf_delete(&dens[i]);
    }
    free(nums); free(dens);

    bf_delete(&xt); bf_delete(&result);
    for (int i = 0; i < count; i++) { bf_delete(&xs[i]); bf_delete(&ys[i]); }
    free(xs); free(ys);
    bf_delete(&prime);
    return rc;

cleanup:
    free(nums); free(dens);
    bf_delete(&xt); bf_delete(&result);
    for (int i = 0; i < count; i++) { bf_delete(&xs[i]); bf_delete(&ys[i]); }
    free(xs); free(ys);
    bf_delete(&prime);
    return -1;
}

int strata_shamir_random(const char *prime_hex, char *out_hex, size_t out_size) {
    if (!out_hex || out_size < 3) return -1;

    bf_context_t *ctx = get_bf_ctx();
    bf_t prime, val;

    bf_init(ctx, &prime);
    bf_init(ctx, &val);

    if (!prime_hex) prime_hex = STRATA_SHAMIR_DEFAULT_PRIME;
    if (hex_to_bf(&prime, prime_hex) != 0) {
        bf_delete(&prime); bf_delete(&val);
        return -1;
    }

    random_below(&val, &prime);
    int rc = bf_to_hex(&val, out_hex, out_size);

    bf_delete(&prime); bf_delete(&val);
    return rc;
}
