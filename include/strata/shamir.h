/*
 * Shamir Secret Sharing — M-of-N threshold scheme.
 *
 * Split a secret into N shares where any M can recover it.
 * Uses libbf (QuickJS bignum) for arbitrary-precision modular arithmetic
 * and libsodium for cryptographic randomness.
 *
 * Default prime: 2^132 - 347 (matches brainwallet).
 * Values are hex strings for JSON protocol compatibility.
 */
#ifndef STRATA_SHAMIR_H
#define STRATA_SHAMIR_H

#include <stddef.h>

#define STRATA_SHAMIR_DEFAULT_PRIME "ffffffffffffffffffffffffffffffea5"
#define STRATA_SHAMIR_DEFAULT_BITS  132

/*
 * Split a secret into shares.
 *
 * secret_hex:  hex string of the secret (must be < prime)
 * minimum:     M — threshold for recovery
 * shares:      N — total shares to create
 * prime_hex:   prime modulus as hex (NULL for default 2^132 - 347)
 * out_keys:    caller-provided array of (shares) char* pointers;
 *              each filled with malloc'd hex string for keys[1..N]
 *
 * Returns 0 on success, -1 on error.
 * Caller must free each out_keys[i].
 */
int strata_shamir_split(const char *secret_hex, int minimum, int shares,
                        const char *prime_hex, char **out_keys);

/*
 * Recover a value from M shares via Lagrange interpolation.
 *
 * indices:     array of 1-based share indices
 * keys_hex:    array of key hex strings (parallel to indices)
 * count:       number of shares provided
 * target:      index to recover (0 = secret)
 * prime_hex:   prime modulus as hex (NULL for default)
 * out_hex:     output buffer for recovered hex value
 * out_size:    size of output buffer (needs ~2*ceil(bits/8)+1 bytes)
 *
 * Returns 0 on success, -1 on error.
 */
int strata_shamir_recover(const int *indices, const char **keys_hex, int count,
                          int target, const char *prime_hex,
                          char *out_hex, size_t out_size);

/*
 * Generate a random value in [0, prime-1] using libsodium.
 *
 * prime_hex:   prime modulus as hex (NULL for default)
 * out_hex:     output buffer
 * out_size:    buffer size
 *
 * Returns 0 on success, -1 on error.
 */
int strata_shamir_random(const char *prime_hex, char *out_hex, size_t out_size);

#endif /* STRATA_SHAMIR_H */
