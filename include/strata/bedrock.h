#ifndef STRATA_BEDROCK_H
#define STRATA_BEDROCK_H

/*
 * Bedrock API for native C dens.
 *
 * Native dens #include this header and call these functions directly.
 * The host process (den.c) injects implementations via tcc_add_symbol()
 * before compiling the den source.
 *
 * All strings are null-terminated. Buffer functions return the number
 * of bytes written (excluding null), or -1 on error/timeout.
 */

/* Logging — writes to stderr */
void bedrock_log(const char *msg);

/* Subscribe to event topics on the SUB socket */
int bedrock_subscribe(const char *filter);

/* Receive an event. Returns payload length, or -1 on timeout. */
int bedrock_receive(char *topic_buf, int topic_cap,
                    char *payload_buf, int payload_cap);

/* Send a JSON request to the store (default REQ socket).
 * Returns response length, or -1 on error. */
int bedrock_request(const char *req_json, char *resp_buf, int resp_cap);

/* Send a JSON request to a specific endpoint (peer REQ socket).
 * Returns response length, or -1 on error. */
int bedrock_request_to(const char *endpoint,
                       const char *req_json, char *resp_buf, int resp_cap);

/* Publish an event on the PUB socket. Returns 0 on success. */
int bedrock_publish(const char *topic, const char *payload);

/* Receive a request on the REP socket. Returns length, or -1 on timeout. */
int bedrock_serve_recv(char *buf, int cap);

/* Send a response on the REP socket. Returns 0 on success. */
int bedrock_serve_send(const char *resp);

/* Execute SQL on the per-den local SQLite database.
 * Returns the number of rows changed. */
int bedrock_db_exec(const char *sql);

/* Query the per-den local SQLite database.
 * Writes a JSON array of row objects to result_buf.
 * Returns length written, or -1 on error. */
int bedrock_db_query(const char *sql, char *result_buf, int result_cap);

#endif
