#ifndef STRATA_VILLAGE_H
#define STRATA_VILLAGE_H

#include <sys/types.h>
#include "strata/agent.h"

/* Result from remote_clone */
typedef struct {
    int ok;
    char agent_rep[256];    /* agent's REP endpoint on remote village */
    char agent_pub[256];    /* agent's PUB endpoint on remote village */
    char error[256];
} strata_clone_result;

/* Relay: bridges local agent bedrock sockets to remote endpoints */
typedef struct strata_relay strata_relay;

/* Local clone — wrapper around strata_agent_spawn for API symmetry */
pid_t strata_clone(strata_agent_host *host, const char *agent_name,
                   const char *event_json, int event_len);

/* Remote clone — send agent to a remote village daemon.
 * village_endpoint:     ZMQ endpoint of the remote village daemon
 * origin_req_endpoint:  store service the relay should forward to
 * event_json/event_len: event payload for the agent
 * result:               output (agent endpoints on remote village) */
int strata_remote_clone(strata_agent_host *host, const char *agent_name,
                        const char *village_endpoint,
                        const char *origin_req_endpoint,
                        const char *event_json, int event_len,
                        strata_clone_result *result);

/* Create a relay bridging local <-> remote bedrock sockets.
 * local_rep_ep / remote_req_ep: REQ/REP relay (store requests)
 * local_pub_ep / remote_sub_ep: SUB/PUB relay (event forwarding, optional) */
strata_relay *strata_relay_create(const char *local_rep_ep,
                                   const char *remote_req_ep,
                                   const char *local_pub_ep,
                                   const char *remote_sub_ep);
void strata_relay_destroy(strata_relay *relay);

/* Run the village daemon: listen for clone requests, spawn agents, run relays.
 * Blocks until SIGINT. */
int strata_village_run(const char *listen_endpoint);

#endif
