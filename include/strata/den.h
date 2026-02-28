#ifndef STRATA_DEN_H
#define STRATA_DEN_H

#include <sys/types.h>
#include <stddef.h>
#include "strata/store.h"

#define STRATA_MAX_DENS 64

typedef enum {
    STRATA_MODE_WASM,    /* WASM den (on_event, fire-and-forget) */
    STRATA_MODE_JS       /* QuickJS den (serve loop, full bedrock) */
} strata_mode;

typedef struct strata_den_def {
    char name[64];
    char den_id[256];       /* identity for privilege checks */
    strata_mode mode;
    /* WASM mode */
    char wasm_path[256];
    unsigned char *wasm_buf;
    size_t wasm_len;
    /* JS mode */
    char js_path[256];
    char *js_source;            /* pre-loaded JS source (CoW across forks) */
    /* ZMQ endpoints */
    char trigger_filter[256];
    char sub_endpoint[256];
    char req_endpoint[256];
    char pub_endpoint[256];     /* PUB bind endpoint (strata publishes here) */
    char rep_endpoint[256];     /* REP bind endpoint (strata serves API here) */
} strata_den_def;

typedef struct strata_den_host strata_den_host;

/* Create/destroy den host */
strata_den_host *strata_den_host_create(void);
void              strata_den_host_free(strata_den_host *host);

/* Set/get store for privilege checks (optional — if NULL, no checks) */
void strata_den_host_set_store(strata_den_host *host, strata_store *store);
strata_store *strata_den_host_get_store(const strata_den_host *host);

/* Register a WASM den */
int strata_den_register(strata_den_host *host,
                        const char *name,
                        const char *wasm_path,
                        const char *trigger_filter,
                        const char *sub_endpoint,
                        const char *req_endpoint);

/* Register a JS den (QuickJS, full bedrock) */
int strata_den_js_register(strata_den_host *host,
                           const char *name,
                           const char *js_path,
                           const char *sub_endpoint,
                           const char *req_endpoint,
                           const char *pub_endpoint,
                           const char *rep_endpoint);

/* Explicitly spawn a den with an event payload */
pid_t strata_den_spawn(strata_den_host *host,
                       const char *den_name,
                       const char *event_json, int event_len);

/* Reap finished child processes (non-blocking) */
int strata_den_host_reap(strata_den_host *host);

/* Find den definition by name */
const strata_den_def *strata_den_host_find(const strata_den_host *host,
                                            const char *name);

/* Register from in-memory buffers (used by village daemon) */
int strata_den_register_wasm_buf(strata_den_host *host,
                                  const char *name,
                                  const unsigned char *wasm_buf, size_t wasm_len,
                                  const char *sub_endpoint,
                                  const char *req_endpoint);

int strata_den_register_js_buf(strata_den_host *host,
                                const char *name,
                                const char *js_source,
                                const char *sub_endpoint,
                                const char *req_endpoint,
                                const char *pub_endpoint,
                                const char *rep_endpoint);

#endif
