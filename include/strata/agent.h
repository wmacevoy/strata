#ifndef STRATA_AGENT_H
#define STRATA_AGENT_H

#include <sys/types.h>
#include <stddef.h>

#define STRATA_MAX_AGENTS 64

typedef enum {
    STRATA_MODE_WASM,    /* WASM agent (on_event, fire-and-forget) */
    STRATA_MODE_JS       /* QuickJS strata (serve loop, full bedrock) */
} strata_mode;

typedef struct strata_agent_def {
    char name[64];
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
} strata_agent_def;

typedef struct strata_agent_host strata_agent_host;

/* Create/destroy agent host */
strata_agent_host *strata_agent_host_create(void);
void               strata_agent_host_free(strata_agent_host *host);

/* Register a WASM agent */
int strata_agent_register(strata_agent_host *host,
                          const char *name,
                          const char *wasm_path,
                          const char *trigger_filter,
                          const char *sub_endpoint,
                          const char *req_endpoint);

/* Register a JS strata (QuickJS, full bedrock) */
int strata_js_register(strata_agent_host *host,
                       const char *name,
                       const char *js_path,
                       const char *sub_endpoint,
                       const char *req_endpoint,
                       const char *pub_endpoint,
                       const char *rep_endpoint);

/* Explicitly spawn an agent/strata with an event payload */
pid_t strata_agent_spawn(strata_agent_host *host,
                         const char *agent_name,
                         const char *event_json, int event_len);

/* Reap finished child processes (non-blocking) */
int strata_agent_host_reap(strata_agent_host *host);

/* Find agent definition by name */
const strata_agent_def *strata_agent_host_find(const strata_agent_host *host,
                                                const char *name);

/* Register from in-memory buffers (used by village daemon) */
int strata_agent_register_wasm_buf(strata_agent_host *host,
                                    const char *name,
                                    const unsigned char *wasm_buf, size_t wasm_len,
                                    const char *sub_endpoint,
                                    const char *req_endpoint);

int strata_agent_register_js_buf(strata_agent_host *host,
                                  const char *name,
                                  const char *js_source,
                                  const char *sub_endpoint,
                                  const char *req_endpoint,
                                  const char *pub_endpoint,
                                  const char *rep_endpoint);

#endif
