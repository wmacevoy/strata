/*
 * Strata change listener example.
 *
 * Usage: ./listener [endpoint] [topic_filter]
 *   endpoint:     ZMQ endpoint to connect to (default: tcp://127.0.0.1:5555)
 *   topic_filter: subscribe filter prefix (default: "change/" = all changes)
 *
 * Examples:
 *   ./listener                                    # all changes on default port
 *   ./listener tcp://127.0.0.1:5555 change/proj-1 # only proj-1 changes
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include "strata/transport.h"

static volatile int running = 1;

static void sigint_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char **argv) {
    const char *endpoint = argc > 1 ? argv[1] : "tcp://127.0.0.1:5555";
    const char *filter   = argc > 2 ? argv[2] : "change/";

    signal(SIGINT, sigint_handler);

    strata_sock sub;
    strata_sub_open(&sub);
    strata_sock_dial(sub, endpoint);
    strata_sock_subscribe(sub, filter, strlen(filter));

    printf("listening on %s (filter: %s)\n", endpoint, filter);
    printf("press Ctrl+C to stop\n\n");

    strata_sock_set_recv_timeout(sub, 1000);

    while (running) {
        char topic[512] = {0};
        char payload[4096] = {0};

        int rc = strata_sub_recv(sub, topic, sizeof(topic), payload, sizeof(payload));
        if (rc < 0) continue;  /* timeout, check running flag */

        printf("[%s] %s\n", topic, payload);
    }

    printf("\nshutting down\n");
    strata_sock_close(sub);
    return 0;
}
