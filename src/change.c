#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <zmq.h>
#include "strata/change.h"

struct strata_change_pub {
    void *zmq_ctx;
    void *socket;
};

strata_change_pub *strata_change_pub_create(const char *endpoint) {
    if (!endpoint) return NULL;

    strata_change_pub *pub = calloc(1, sizeof(*pub));
    if (!pub) return NULL;

    pub->zmq_ctx = zmq_ctx_new();
    if (!pub->zmq_ctx) {
        free(pub);
        return NULL;
    }

    pub->socket = zmq_socket(pub->zmq_ctx, ZMQ_PUB);
    if (!pub->socket) {
        zmq_ctx_destroy(pub->zmq_ctx);
        free(pub);
        return NULL;
    }

    if (zmq_bind(pub->socket, endpoint) != 0) {
        fprintf(stderr, "zmq_bind failed: %s\n", zmq_strerror(zmq_errno()));
        zmq_close(pub->socket);
        zmq_ctx_destroy(pub->zmq_ctx);
        free(pub);
        return NULL;
    }

    return pub;
}

void strata_change_pub_free(strata_change_pub *pub) {
    if (!pub) return;
    if (pub->socket) zmq_close(pub->socket);
    if (pub->zmq_ctx) zmq_ctx_destroy(pub->zmq_ctx);
    free(pub);
}

int strata_change_publish(strata_change_pub *pub,
                          const char *repo_id,
                          const char *artifact_id,
                          const char *artifact_type,
                          const char *change_type,
                          const char *author) {
    if (!pub || !pub->socket) return -1;

    /* Topic: change/{repo_id}/{artifact_type} */
    char topic[512];
    snprintf(topic, sizeof(topic), "change/%s/%s", repo_id, artifact_type);

    /* Payload: simple JSON */
    char payload[1024];
    snprintf(payload, sizeof(payload),
        "{\"repo_id\":\"%s\",\"artifact_id\":\"%s\","
        "\"artifact_type\":\"%s\",\"change\":\"%s\",\"author\":\"%s\"}",
        repo_id, artifact_id, artifact_type, change_type, author);

    /* ZMQ multipart: topic frame + payload frame */
    zmq_send(pub->socket, topic, strlen(topic), ZMQ_SNDMORE);
    zmq_send(pub->socket, payload, strlen(payload), 0);

    return 0;
}
