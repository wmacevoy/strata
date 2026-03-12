/*
 * AEAD encryption for strata — XChaCha20-Poly1305 via libsodium.
 *
 * Wire format: "AE02" (4) || nonce (24) || ciphertext+tag (N+16)
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sodium.h>
#include <zmq.h>
#include "strata/aead.h"

static const uint8_t AEAD_MAGIC[4] = {'A', 'E', '0', '2'};

static void ensure_sodium_init(void) {
    static int done = 0;
    if (!done) {
        if (sodium_init() < 0) {
            fprintf(stderr, "strata: sodium_init() failed\n");
            abort();
        }
        done = 1;
    }
}

/* ------------------------------------------------------------------ */
/*  Key lifecycle                                                      */
/* ------------------------------------------------------------------ */

int strata_aead_keygen(strata_aead_key *key) {
    if (!key) return -1;
    ensure_sodium_init();
    randombytes_buf(key->bytes, STRATA_KEY_LEN);
    return 0;
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
    ensure_sodium_init();

    /* Extract: PRK = HMAC-SHA256(salt="strata-aead", IKM=master) */
    const char *salt = "strata-aead";
    uint8_t prk[crypto_kdf_hkdf_sha256_KEYBYTES];
    if (crypto_kdf_hkdf_sha256_extract(prk,
            (const unsigned char *)salt, strlen(salt),
            master->bytes, STRATA_KEY_LEN) != 0)
        return -1;

    /* Expand: OKM = HKDF-Expand(PRK, info, 32) */
    if (crypto_kdf_hkdf_sha256_expand(derived->bytes, STRATA_KEY_LEN,
            info, strlen(info), prk) != 0)
        return -1;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  XChaCha20-Poly1305 seal / open                                     */
/* ------------------------------------------------------------------ */

int strata_aead_seal(const strata_aead_key *key,
                     const uint8_t *plaintext, size_t plaintext_len,
                     const uint8_t *aad, size_t aad_len,
                     uint8_t *out, size_t *out_len) {
    if (!key || !out || !out_len) return -1;
    if (!plaintext && plaintext_len > 0) return -1;
    ensure_sodium_init();

    /* Layout: magic(4) || nonce(24) || ciphertext+tag(N+16) */
    uint8_t *magic = out;
    uint8_t *nonce = out + STRATA_MAGIC_LEN;
    uint8_t *ct    = nonce + STRATA_NONCE_LEN;

    memcpy(magic, AEAD_MAGIC, STRATA_MAGIC_LEN);
    randombytes_buf(nonce, STRATA_NONCE_LEN);

    unsigned long long clen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            ct, &clen,
            plaintext, (unsigned long long)plaintext_len,
            aad, (unsigned long long)aad_len,
            NULL, nonce, key->bytes) != 0)
        return -1;

    *out_len = STRATA_MAGIC_LEN + STRATA_NONCE_LEN + (size_t)clen;
    return 0;
}

int strata_aead_open(const strata_aead_key *key,
                     const uint8_t *sealed, size_t sealed_len,
                     const uint8_t *aad, size_t aad_len,
                     uint8_t *out, size_t *out_len) {
    if (!key || !sealed || !out || !out_len) return -1;
    if (sealed_len < STRATA_OVERHEAD) return -1;
    if (memcmp(sealed, AEAD_MAGIC, STRATA_MAGIC_LEN) != 0) return -1;
    ensure_sodium_init();

    const uint8_t *nonce = sealed + STRATA_MAGIC_LEN;
    const uint8_t *ct = nonce + STRATA_NONCE_LEN;
    size_t ct_len = sealed_len - STRATA_MAGIC_LEN - STRATA_NONCE_LEN;

    unsigned long long mlen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            out, &mlen,
            NULL,
            ct, (unsigned long long)ct_len,
            aad, (unsigned long long)aad_len,
            nonce, key->bytes) != 0)
        return -1;

    *out_len = (size_t)mlen;
    return 0;
}

int strata_aead_is_sealed(const uint8_t *data, size_t len) {
    if (!data || len < STRATA_OVERHEAD) return 0;
    return memcmp(data, AEAD_MAGIC, STRATA_MAGIC_LEN) == 0;
}

/* ------------------------------------------------------------------ */
/*  ZMQ transport encryption (message-level AEAD)                      */
/* ------------------------------------------------------------------ */

static strata_aead_key g_transport_key;
static int g_transport_key_init = 0;

strata_aead_key *strata_transport_key(void) {
    if (!g_transport_key_init) {
        strata_aead_key bedrock;
        if (strata_aead_key_from_env(&bedrock) != 0) return NULL;
        strata_aead_derive(&bedrock, "zmq-transport", &g_transport_key);
        g_transport_key_init = 1;
    }
    return &g_transport_key;
}

int strata_zmq_send(void *sock, const void *buf, size_t len, int flags) {
    strata_aead_key *tk = strata_transport_key();
    if (!tk) return zmq_send(sock, buf, len, flags);

    size_t enc_len = len + STRATA_OVERHEAD;
    uint8_t *enc = malloc(enc_len);
    if (!enc) return -1;

    if (strata_aead_seal(tk, buf, len, NULL, 0, enc, &enc_len) != 0) {
        free(enc);
        return -1;
    }
    int rc = zmq_send(sock, enc, enc_len, flags);
    free(enc);
    return rc;
}

int strata_zmq_recv(void *sock, void *buf, size_t len, int flags) {
    /* Receive into a temp buffer large enough for the sealed message */
    size_t tmp_len = len + STRATA_OVERHEAD;
    uint8_t *tmp = malloc(tmp_len);
    if (!tmp) return -1;

    int rc = zmq_recv(sock, tmp, tmp_len, flags);
    if (rc < 0) { free(tmp); return rc; }

    /* If not sealed, return plaintext as-is (backward compatible) */
    if (!strata_aead_is_sealed(tmp, rc)) {
        if ((size_t)rc > len) rc = (int)len;
        memcpy(buf, tmp, rc);
        free(tmp);
        return rc;
    }

    /* Decrypt */
    strata_aead_key *tk = strata_transport_key();
    if (!tk) { free(tmp); return -1; }

    size_t dec_len = 0;
    if (strata_aead_open(tk, tmp, rc, NULL, 0, buf, &dec_len) != 0) {
        free(tmp);
        return -1;
    }
    free(tmp);
    return (int)dec_len;
}
