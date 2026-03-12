#ifndef STRATA_AEAD_H
#define STRATA_AEAD_H

#include <stdint.h>
#include <stddef.h>

#define STRATA_KEY_LEN     32   /* AES-256 */
#define STRATA_NONCE_LEN   12   /* GCM standard */
#define STRATA_GCM_TAG_LEN 16   /* GCM auth tag */
#define STRATA_MAGIC_LEN    4   /* "AE01" version marker */
#define STRATA_OVERHEAD    (STRATA_MAGIC_LEN + STRATA_NONCE_LEN + STRATA_GCM_TAG_LEN)

/* Wire format: "AE01" (4) || nonce (12) || ciphertext (N) || gcm_tag (16)
 * Total overhead: 32 bytes.
 * The magic header distinguishes encrypted from plaintext blobs. */

typedef struct {
    uint8_t bytes[STRATA_KEY_LEN];
} strata_aead_key;

/* Generate a random 256-bit key */
int strata_aead_keygen(strata_aead_key *key);

/* Load key from hex string (64 hex chars) */
int strata_aead_key_from_hex(strata_aead_key *key, const char *hex);

/* Write key as hex string (needs 65 bytes including null) */
int strata_aead_key_to_hex(const strata_aead_key *key, char *hex, size_t cap);

/* Load raw 32-byte key from file */
int strata_aead_key_load(strata_aead_key *key, const char *path);

/* Save raw 32-byte key to file (mode 0600) */
int strata_aead_key_save(const strata_aead_key *key, const char *path);

/* Derive a sub-key via HKDF-SHA256.
 * info is context (e.g. blob tag, "transport", den name). */
int strata_aead_derive(const strata_aead_key *master,
                       const char *info,
                       strata_aead_key *derived);

/* Encrypt (seal) plaintext with AAD.
 * out must be at least plaintext_len + STRATA_OVERHEAD bytes.
 * aad/aad_len: additional authenticated data (e.g. blob tag).
 * Returns 0 on success, -1 on failure. */
int strata_aead_seal(const strata_aead_key *key,
                     const uint8_t *plaintext, size_t plaintext_len,
                     const uint8_t *aad, size_t aad_len,
                     uint8_t *out, size_t *out_len);

/* Decrypt (open) sealed data with AAD verification.
 * out must be at least sealed_len - STRATA_OVERHEAD bytes.
 * Returns 0 on success, -1 on auth failure or error. */
int strata_aead_open(const strata_aead_key *key,
                     const uint8_t *sealed, size_t sealed_len,
                     const uint8_t *aad, size_t aad_len,
                     uint8_t *out, size_t *out_len);

/* Check if data starts with the AEAD magic header ("AE01"). */
int strata_aead_is_sealed(const uint8_t *data, size_t len);

/* Load bedrock key from environment:
 *   STRATA_BEDROCK_KEY      — 64 hex chars
 *   STRATA_BEDROCK_KEY_FILE — path to raw 32-byte file
 * Returns 0 on success, -1 if neither is set. */
int strata_aead_key_from_env(strata_aead_key *key);

#endif
