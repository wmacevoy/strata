#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <nng/nng.h>
#include "strata/transport.h"
#include "strata/change.h"

struct strata_change_pub {
    strata_sock socket;
};

strata_change_pub *strata_change_pub_create(const char *endpoint) {
    if (!endpoint) return NULL;

    strata_change_pub *pub = calloc(1, sizeof(*pub));
    if (!pub) return NULL;

    memset(&pub->socket, 0, sizeof(pub->socket));

    int rv = strata_pub_open(&pub->socket);
    if (rv != 0) {
        fprintf(stderr, "strata_pub_open failed: %s\n", nng_strerror(rv));
        free(pub);
        return NULL;
    }

    rv = strata_sock_bind(pub->socket, endpoint);
    if (rv != 0) {
        fprintf(stderr, "strata_sock_bind failed: %s\n", nng_strerror(rv));
        strata_sock_close(pub->socket);
        free(pub);
        return NULL;
    }

    return pub;
}

void strata_change_pub_free(strata_change_pub *pub) {
    if (!pub) return;
    if (strata_sock_valid(pub->socket)) strata_sock_close(pub->socket);
    free(pub);
}

int strata_change_publish(strata_change_pub *pub,
                          const char *repo_id,
                          const char *artifact_id,
                          const char *artifact_type,
                          const char *change_type,
                          const char *author) {
    if (!pub || !strata_sock_valid(pub->socket)) return -1;

    /* Topic: change/{repo_id}/{artifact_type} */
    char topic[512];
    snprintf(topic, sizeof(topic), "change/%s/%s", repo_id, artifact_type);

    /* Payload: simple JSON */
    char payload[1024];
    snprintf(payload, sizeof(payload),
        "{\"repo_id\":\"%s\",\"artifact_id\":\"%s\","
        "\"artifact_type\":\"%s\",\"change\":\"%s\",\"author\":\"%s\"}",
        repo_id, artifact_id, artifact_type, change_type, author);

    /* PUB send: topic + payload in single framed message */
    int rv = strata_pub_send(pub->socket, topic, payload);
    if (rv < 0) return -1;

    return 0;
}
