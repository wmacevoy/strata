#ifndef STRATA_TRANSPORT_H
#define STRATA_TRANSPORT_H

/*
 * Transport abstraction — nng underneath, zmq-style return conventions.
 *
 * REQ/REP channels use message-level AEAD encryption (XChaCha20-Poly1305)
 * when a bedrock key is set (STRATA_BEDROCK_KEY env var).
 * PUB/SUB stays plaintext (prefix matching requires readable topic bytes).
 *
 * PUB/SUB framing: single message = topic + '\n' + payload.
 * Subscribers filter on topic prefix; strata_sub_recv splits at first '\n'.
 */

#include <stddef.h>
#include <nng/nng.h>

/* Socket wrapper — pass by value (just a uint32_t id inside) */
typedef nng_socket strata_sock;

/* Invalid / uninitialized sentinel — use only in declarations, not assignments.
 * For assignments, use: memset(&s, 0, sizeof(s)) */
#define STRATA_SOCK_INIT NNG_SOCKET_INITIALIZER

static inline int strata_sock_valid(strata_sock s) {
    return nng_socket_id(s) >= 0;
}

/* Open typed sockets */
int strata_req_open(strata_sock *s);
int strata_rep_open(strata_sock *s);
int strata_pub_open(strata_sock *s);
int strata_sub_open(strata_sock *s);

/* Lifecycle */
int  strata_sock_bind(strata_sock s, const char *url);
int  strata_sock_dial(strata_sock s, const char *url);
void strata_sock_close(strata_sock s);

/* Options */
int strata_sock_set_recv_timeout(strata_sock s, int ms);
int strata_sock_set_send_timeout(strata_sock s, int ms);
int strata_sock_set_linger(strata_sock s, int ms);
int strata_sock_subscribe(strata_sock s, const char *topic, size_t len);

/* Encrypted send/recv — AEAD on REQ/REP, returns bytes or -1 */
int strata_send(strata_sock s, const void *buf, size_t len);
int strata_recv(strata_sock s, void *buf, size_t cap);

/* PUB/SUB with topic framing (plaintext, no AEAD) */
int strata_pub_send(strata_sock s, const char *topic, const char *payload);
int strata_sub_recv(strata_sock s, char *topic, int topic_cap,
                    char *payload, int payload_cap);

/* Raw send/recv — no AEAD, for relay/plaintext paths */
int strata_raw_send(strata_sock s, const void *buf, size_t len);
int strata_raw_recv(strata_sock s, void *buf, size_t cap);

/* Multipart framing for village clone (single message, length-prefixed) */
int strata_multipart_send(strata_sock s, const char **parts, const size_t *lens, int nparts);
int strata_multipart_recv(strata_sock s, char **parts, size_t *lens, size_t *caps, int nparts);

#endif
