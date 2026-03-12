/*
 * AEAD encryption for strata — AES-256-GCM with tagged AAD.
 *
 * Wire format: "AE01" (4) || nonce (12) || ciphertext (N) || gcm_tag (16)
 *
 * Uses OpenSSL EVP on all platforms.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include "strata/aead.h"

static const uint8_t AEAD_MAGIC[4] = {'A', 'E', '0', '1'};

/* ------------------------------------------------------------------ */
/*  Key lifecycle                                                      */
/* ------------------------------------------------------------------ */

int strata_aead_keygen(strata_aead_key *key) {
    if (!key) return -1;
    return (RAND_bytes(key->bytes, STRATA_KEY_LEN) == 1) ? 0 : -1;
}

static int hex_to_byte(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int strata_aead_key_from_hex(strata_aead_key *key, const char *hex) {
    if (!key || !hex || strlen(hex) < STRATA_KEY_LEN * 2) return -1;
    for (int i = 0; i < STRATA_KEY_LEN; i++) {
        int hi = hex_to_byte(hex[i * 2]);
        int lo = hex_to_byte(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        key->bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

int strata_aead_key_to_hex(const strata_aead_key *key, char *hex, size_t cap) {
    if (!key || !hex || cap < STRATA_KEY_LEN * 2 + 1) return -1;
    for (int i = 0; i < STRATA_KEY_LEN; i++)
        sprintf(hex + i * 2, "%02x", key->bytes[i]);
    hex[STRATA_KEY_LEN * 2] = '\0';
    return 0;
}

int strata_aead_key_load(strata_aead_key *key, const char *path) {
    if (!key || !path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(key->bytes, 1, STRATA_KEY_LEN, f);
    fclose(f);
    return (n == STRATA_KEY_LEN) ? 0 : -1;
}

int strata_aead_key_save(const strata_aead_key *key, const char *path) {
    if (!key || !path) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); return -1; }
    size_t n = fwrite(key->bytes, 1, STRATA_KEY_LEN, f);
    fclose(f);
    return (n == STRATA_KEY_LEN) ? 0 : -1;
}

int strata_aead_key_from_env(strata_aead_key *key) {
    if (!key) return -1;
    const char *hex = getenv("STRATA_BEDROCK_KEY");
    if (hex && strlen(hex) >= STRATA_KEY_LEN * 2)
        return strata_aead_key_from_hex(key, hex);
    const char *path = getenv("STRATA_BEDROCK_KEY_FILE");
    if (path)
        return strata_aead_key_load(key, path);
    return -1;
}

/* ------------------------------------------------------------------ */
/*  HKDF-SHA256 (extract + expand, single 32-byte output)             */
/* ------------------------------------------------------------------ */

int strata_aead_derive(const strata_aead_key *master,
                       const char *info,
                       strata_aead_key *derived) {
    if (!master || !info || !derived) return -1;

    /* HKDF extract: PRK = HMAC-SHA256(salt="strata-aead", IKM=master) */
    const char *salt = "strata-aead";
    unsigned int prk_len = 32;
    uint8_t prk[32];
    HMAC(EVP_sha256(), salt, (int)strlen(salt),
         master->bytes, STRATA_KEY_LEN, prk, &prk_len);

    /* HKDF expand: OKM = HMAC-SHA256(PRK, info || 0x01)
     * We only need 32 bytes — one HMAC block. */
    size_t info_len = strlen(info);
    uint8_t *expand_input = malloc(info_len + 1);
    if (!expand_input) return -1;
    memcpy(expand_input, info, info_len);
    expand_input[info_len] = 0x01;

    unsigned int okm_len = 32;
    HMAC(EVP_sha256(), prk, 32,
         expand_input, info_len + 1, derived->bytes, &okm_len);
    free(expand_input);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  AES-256-GCM seal / open                                            */
/* ------------------------------------------------------------------ */

int strata_aead_seal(const strata_aead_key *key,
                     const uint8_t *plaintext, size_t plaintext_len,
                     const uint8_t *aad, size_t aad_len,
                     uint8_t *out, size_t *out_len) {
    if (!key || !out || !out_len) return -1;
    if (!plaintext && plaintext_len > 0) return -1;

    size_t total = STRATA_MAGIC_LEN + STRATA_NONCE_LEN + plaintext_len + STRATA_GCM_TAG_LEN;

    /* Layout: magic(4) || nonce(12) || ciphertext(N) || tag(16) */
    uint8_t *magic = out;
    uint8_t *nonce = out + STRATA_MAGIC_LEN;
    uint8_t *ct    = nonce + STRATA_NONCE_LEN;
    uint8_t *tag   = ct + plaintext_len;

    memcpy(magic, AEAD_MAGIC, STRATA_MAGIC_LEN);
    if (RAND_bytes(nonce, STRATA_NONCE_LEN) != 1) return -1;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    int ok = 1;
    int len;

    ok = ok && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL);
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, STRATA_NONCE_LEN, NULL);
    ok = ok && EVP_EncryptInit_ex(ctx, NULL, NULL, key->bytes, nonce);
    if (aad && aad_len > 0)
        ok = ok && EVP_EncryptUpdate(ctx, NULL, &len, aad, (int)aad_len);
    if (plaintext_len > 0)
        ok = ok && EVP_EncryptUpdate(ctx, ct, &len, plaintext, (int)plaintext_len);
    ok = ok && EVP_EncryptFinal_ex(ctx, ct + (plaintext_len > 0 ? (size_t)len : 0), &len);
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, STRATA_GCM_TAG_LEN, tag);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return -1;

    *out_len = total;
    return 0;
}

int strata_aead_open(const strata_aead_key *key,
                     const uint8_t *sealed, size_t sealed_len,
                     const uint8_t *aad, size_t aad_len,
                     uint8_t *out, size_t *out_len) {
    if (!key || !sealed || !out || !out_len) return -1;
    if (sealed_len < STRATA_OVERHEAD) return -1;
    if (memcmp(sealed, AEAD_MAGIC, STRATA_MAGIC_LEN) != 0) return -1;

    const uint8_t *nonce = sealed + STRATA_MAGIC_LEN;
    size_t ct_len = sealed_len - STRATA_OVERHEAD;
    const uint8_t *ct = nonce + STRATA_NONCE_LEN;
    const uint8_t *tag = ct + ct_len;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    int ok = 1;
    int len;

    ok = ok && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL);
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, STRATA_NONCE_LEN, NULL);
    ok = ok && EVP_DecryptInit_ex(ctx, NULL, NULL, key->bytes, nonce);
    if (aad && aad_len > 0)
        ok = ok && EVP_DecryptUpdate(ctx, NULL, &len, aad, (int)aad_len);
    if (ct_len > 0)
        ok = ok && EVP_DecryptUpdate(ctx, out, &len, ct, (int)ct_len);
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                                    STRATA_GCM_TAG_LEN, (void *)tag);
    ok = ok && (EVP_DecryptFinal_ex(ctx, out + (ct_len > 0 ? (size_t)len : 0), &len) > 0);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return -1;

    *out_len = ct_len;
    return 0;
}

int strata_aead_is_sealed(const uint8_t *data, size_t len) {
    if (!data || len < STRATA_OVERHEAD) return 0;
    return memcmp(data, AEAD_MAGIC, STRATA_MAGIC_LEN) == 0;
}
