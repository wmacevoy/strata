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
#include <zmq.h>

static volatile int running = 1;

static void sigint_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char **argv) {
    const char *endpoint = argc > 1 ? argv[1] : "tcp://127.0.0.1:5555";
    const char *filter   = argc > 2 ? argv[2] : "change/";

    signal(SIGINT, sigint_handler);

    void *ctx = zmq_ctx_new();
    void *sub = zmq_socket(ctx, ZMQ_SUB);
    zmq_connect(sub, endpoint);
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, filter, strlen(filter));

    printf("listening on %s (filter: %s)\n", endpoint, filter);
    printf("press Ctrl+C to stop\n\n");

    int timeout = 1000;
    zmq_setsockopt(sub, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));

    while (running) {
        char topic[512] = {0};
        char payload[4096] = {0};

        int rc = zmq_recv(sub, topic, sizeof(topic) - 1, 0);
        if (rc < 0) continue;  /* timeout, check running flag */

        rc = zmq_recv(sub, payload, sizeof(payload) - 1, 0);
        if (rc < 0) continue;

        printf("[%s] %s\n", topic, payload);
    }

    printf("\nshutting down\n");
    zmq_close(sub);
    zmq_ctx_destroy(ctx);
    return 0;
}
