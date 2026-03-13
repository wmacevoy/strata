/*
 * strata/msg.h — Minimal TCP transport for strata.
 *
 * Replaces ZeroMQ with ~400 lines of C. Pure C11, fork-safe,
 * no threads, no external dependencies.
 *
 * Patterns:
 *   REQ/REP — connect, send request, recv response (or bind, recv, send)
 *   PUB/SUB — bind publisher, subscribers connect, topic-filtered broadcast
 *
 * Wire format: [4-byte big-endian length] [payload]
 *   length = number of payload bytes (uint32_t, network byte order)
 *
 * PUB/SUB wire format: [4-byte topic_len] [topic] [4-byte payload_len] [payload]
 *   Topic filtering is server-side (publisher checks subscriber's prefix).
 */
#ifndef STRATA_MSG_H
#define STRATA_MSG_H

#include <stddef.h>
#include <stdint.h>

/* Opaque socket handle */
typedef struct strata_sock strata_sock;

/* Opaque publisher hub (manages subscriber list) */
typedef struct strata_pub_hub strata_pub_hub;

/* ---- REQ/REP ---- */

/* Bind a REP socket (listen for connections).
 * endpoint: "tcp://host:port" — parsed internally.
 * Returns socket or NULL on error. */
strata_sock *strata_rep_bind(const char *endpoint);

/* Accept next client on a REP socket (blocking).
 * Call this in a serve loop. The returned socket is used for
 * one recv+send exchange, then closed or reused.
 * For single-client REP (like store_service), call once.
 * Returns NULL on error/timeout. */
strata_sock *strata_rep_accept(strata_sock *rep);

/* Connect a REQ socket to a REP server.
 * Returns socket or NULL on error. */
strata_sock *strata_req_connect(const char *endpoint);

/* Send a message. Returns bytes sent or -1 on error. */
int strata_msg_send(strata_sock *sock, const void *buf, size_t len, int flags);

/* Receive a message. Returns bytes received or -1 on timeout/error.
 * Writes at most cap bytes into buf. */
int strata_msg_recv(strata_sock *sock, void *buf, size_t cap, int flags);

/* Set receive timeout in milliseconds (-1 = block forever). */
void strata_msg_set_timeout(strata_sock *sock, int recv_ms, int send_ms);

/* Close and free socket. */
void strata_sock_close(strata_sock *sock);

/* Get the raw file descriptor (for poll/select). */
int strata_sock_fd(strata_sock *sock);

/* ---- PUB/SUB ---- */

/* Create a PUB hub that listens on endpoint.
 * Subscribers connect to this endpoint. */
strata_pub_hub *strata_pub_bind(const char *endpoint);

/* Publish topic+payload to all matching subscribers.
 * Returns number of subscribers reached, or -1 on error. */
int strata_pub_send(strata_pub_hub *pub,
                    const char *topic, size_t topic_len,
                    const void *payload, size_t payload_len);

/* Destroy pub hub, close all subscriber connections. */
void strata_pub_close(strata_pub_hub *pub);

/* Connect a SUB socket to a PUB endpoint. */
strata_sock *strata_sub_connect(const char *endpoint);

/* Subscribe to a topic prefix (empty string = all topics). */
int strata_sub_subscribe(strata_sock *sock, const char *prefix);

/* Receive next matching message.
 * topic_buf: receives the topic (null-terminated).
 * payload_buf: receives the payload.
 * Returns payload length, or -1 on timeout/error. */
int strata_sub_recv(strata_sock *sock,
                    char *topic_buf, size_t topic_cap,
                    void *payload_buf, size_t payload_cap);

/* ---- Endpoint parsing ---- */

/* Parse "tcp://host:port" → host string + port number.
 * Returns 0 on success. host_buf must be at least 256 bytes. */
int strata_endpoint_parse(const char *endpoint,
                          char *host_buf, size_t host_cap,
                          int *port);

#endif /* STRATA_MSG_H */
