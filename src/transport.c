/*
 * Transport layer — nng implementation with AEAD encryption.
 *
 * REQ/REP: message-level AEAD (XChaCha20-Poly1305) via strata_send/recv.
 * PUB/SUB: plaintext, topic\npayload framing for prefix-match filtering.
 * Relay:   raw send/recv (transparent byte forwarding).
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <nng/nng.h>
#include <nng/protocol/reqrep0/req.h>
#include <nng/protocol/reqrep0/rep.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

#include "strata/transport.h"
#include "strata/aead.h"

/* ------------------------------------------------------------------ */
/*  Socket lifecycle                                                    */
/* ------------------------------------------------------------------ */

int strata_req_open(strata_sock *s) { return nng_req0_open(s); }
int strata_rep_open(strata_sock *s) { return nng_rep0_open(s); }
int strata_pub_open(strata_sock *s) { return nng_pub0_open(s); }
int strata_sub_open(strata_sock *s) { return nng_sub0_open(s); }

int strata_sock_bind(strata_sock s, const char *url) {
    return nng_listen(s, url, NULL, 0);
}

int strata_sock_dial(strata_sock s, const char *url) {
    return nng_dial(s, url, NULL, 0);
}

void strata_sock_close(strata_sock s) {
    nng_close(s);
}

/* ------------------------------------------------------------------ */
/*  Options                                                             */
/* ------------------------------------------------------------------ */

int strata_sock_set_recv_timeout(strata_sock s, int ms) {
    return nng_socket_set_ms(s, NNG_OPT_RECVTIMEO, (nng_duration)ms);
}

int strata_sock_set_send_timeout(strata_sock s, int ms) {
    return nng_socket_set_ms(s, NNG_OPT_SENDTIMEO, (nng_duration)ms);
}

int strata_sock_set_linger(strata_sock s, int ms) {
    /* nng doesn't have a direct linger equivalent — close is immediate.
     * This is a no-op for API compatibility. */
    (void)s; (void)ms;
    return 0;
}

int strata_sock_subscribe(strata_sock s, const char *topic, size_t len) {
    return nng_socket_set(s, NNG_OPT_SUB_SUBSCRIBE, topic, len);
}

/* ------------------------------------------------------------------ */
/*  Raw send/recv (no encryption)                                       */
/* ------------------------------------------------------------------ */

int strata_raw_send(strata_sock s, const void *buf, size_t len) {
    int rv = nng_send(s, (void *)buf, len, 0);
    return rv == 0 ? (int)len : -1;
}

int strata_raw_recv(strata_sock s, void *buf, size_t cap) {
    size_t sz = cap;
    int rv = nng_recv(s, buf, &sz, 0);
    return rv == 0 ? (int)sz : -1;
}

/* ------------------------------------------------------------------ */
/*  Encrypted send/recv (AEAD on REQ/REP)                               */
/* ------------------------------------------------------------------ */

int strata_send(strata_sock s, const void *buf, size_t len) {
    strata_aead_key *tk = strata_transport_key();
    if (!tk) return strata_raw_send(s, buf, len);

    size_t enc_len = len + STRATA_OVERHEAD;
    uint8_t *enc = malloc(enc_len);
    if (!enc) return -1;

    if (strata_aead_seal(tk, buf, len, NULL, 0, enc, &enc_len) != 0) {
        free(enc);
        return -1;
    }
    int rc = strata_raw_send(s, enc, enc_len);
    free(enc);
    /* Return original plaintext length on success */
    return rc >= 0 ? (int)len : -1;
}

int strata_recv(strata_sock s, void *buf, size_t cap) {
    size_t tmp_len = cap + STRATA_OVERHEAD;
    uint8_t *tmp = malloc(tmp_len);
    if (!tmp) return -1;

    int rc = strata_raw_recv(s, tmp, tmp_len);
    if (rc < 0) { free(tmp); return rc; }

    /* If not sealed, return plaintext as-is (backward compatible) */
    if (!strata_aead_is_sealed(tmp, rc)) {
        if ((size_t)rc > cap) rc = (int)cap;
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

/* ------------------------------------------------------------------ */
/*  PUB/SUB with topic framing                                          */
/* ------------------------------------------------------------------ */

int strata_pub_send(strata_sock s, const char *topic, const char *payload) {
    if (!topic || !payload) return -1;
    size_t tlen = strlen(topic);
    size_t plen = strlen(payload);
    size_t total = tlen + 1 + plen;  /* topic + '\n' + payload */

    char *msg = malloc(total);
    if (!msg) return -1;
    memcpy(msg, topic, tlen);
    msg[tlen] = '\n';
    memcpy(msg + tlen + 1, payload, plen);

    int rc = strata_raw_send(s, msg, total);
    free(msg);
    return rc;
}

int strata_sub_recv(strata_sock s, char *topic, int topic_cap,
                    char *payload, int payload_cap) {
    /* Receive full message, split at first '\n' */
    size_t buf_cap = (size_t)topic_cap + (size_t)payload_cap + 1;
    char *buf = malloc(buf_cap);
    if (!buf) return -1;

    int rc = strata_raw_recv(s, buf, buf_cap);
    if (rc < 0) { free(buf); return -1; }

    /* Find separator */
    char *sep = memchr(buf, '\n', rc);
    if (!sep) {
        /* No separator — entire message is topic, empty payload */
        int tlen = rc < topic_cap - 1 ? rc : topic_cap - 1;
        memcpy(topic, buf, tlen);
        topic[tlen] = '\0';
        payload[0] = '\0';
        free(buf);
        return rc;
    }

    int tlen = (int)(sep - buf);
    int plen = rc - tlen - 1;

    if (tlen >= topic_cap) tlen = topic_cap - 1;
    memcpy(topic, buf, tlen);
    topic[tlen] = '\0';

    if (plen >= payload_cap) plen = payload_cap - 1;
    if (plen > 0) memcpy(payload, sep + 1, plen);
    payload[plen > 0 ? plen : 0] = '\0';

    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Multipart framing (village clone)                                   */
/*  Wire: [4-byte len][data] for each part, concatenated.              */
/* ------------------------------------------------------------------ */

int strata_multipart_send(strata_sock s, const char **parts,
                          const size_t *lens, int nparts) {
    /* Calculate total size */
    size_t total = 0;
    for (int i = 0; i < nparts; i++)
        total += 4 + lens[i];

    uint8_t *buf = malloc(total);
    if (!buf) return -1;

    size_t off = 0;
    for (int i = 0; i < nparts; i++) {
        uint32_t len32 = (uint32_t)lens[i];
        buf[off++] = (len32 >> 24) & 0xFF;
        buf[off++] = (len32 >> 16) & 0xFF;
        buf[off++] = (len32 >>  8) & 0xFF;
        buf[off++] = (len32      ) & 0xFF;
        memcpy(buf + off, parts[i], lens[i]);
        off += lens[i];
    }

    int rc = strata_raw_send(s, buf, total);
    free(buf);
    return rc;
}

int strata_multipart_recv(strata_sock s, char **parts, size_t *lens,
                          size_t *caps, int nparts) {
    /* Receive the whole message */
    size_t total_cap = 0;
    for (int i = 0; i < nparts; i++) total_cap += 4 + caps[i];

    uint8_t *buf = malloc(total_cap);
    if (!buf) return -1;

    int rc = strata_raw_recv(s, buf, total_cap);
    if (rc < 0) { free(buf); return -1; }

    /* Parse length-prefixed parts */
    size_t off = 0;
    for (int i = 0; i < nparts; i++) {
        if (off + 4 > (size_t)rc) {
            /* Not enough data for this part — fill with empty */
            lens[i] = 0;
            if (parts[i] && caps[i] > 0) parts[i][0] = '\0';
            continue;
        }
        uint32_t len32 = ((uint32_t)buf[off] << 24) |
                          ((uint32_t)buf[off+1] << 16) |
                          ((uint32_t)buf[off+2] << 8) |
                          ((uint32_t)buf[off+3]);
        off += 4;

        size_t copy = len32;
        if (copy > caps[i]) copy = caps[i];
        if (off + copy > (size_t)rc) copy = (size_t)rc - off;
        if (parts[i]) memcpy(parts[i], buf + off, copy);
        lens[i] = copy;
        off += len32;
    }

    free(buf);
    return rc;
}
