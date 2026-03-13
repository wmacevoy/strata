#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "strata/msg.h"
#include "strata/change.h"

struct strata_change_pub {
    strata_pub_hub *hub;
};

strata_change_pub *strata_change_pub_create(const char *endpoint) {
    if (!endpoint) return NULL;

    strata_change_pub *pub = calloc(1, sizeof(*pub));
    if (!pub) return NULL;

    pub->hub = strata_pub_bind(endpoint);
    if (!pub->hub) {
        free(pub);
        return NULL;
    }

    return pub;
}

void strata_change_pub_free(strata_change_pub *pub) {
    if (!pub) return;
    if (pub->hub) strata_pub_close(pub->hub);
    free(pub);
}

int strata_change_publish(strata_change_pub *pub,
                          const char *repo_id,
                          const char *artifact_id,
                          const char *artifact_type,
                          const char *change_type,
                          const char *author) {
    if (!pub || !pub->hub) return -1;

    /* Topic: change/{repo_id}/{artifact_type} */
    char topic[512];
    snprintf(topic, sizeof(topic), "change/%s/%s", repo_id, artifact_type);

    /* Payload: simple JSON */
    char payload[1024];
    snprintf(payload, sizeof(payload),
        "{\"repo_id\":\"%s\",\"artifact_id\":\"%s\","
        "\"artifact_type\":\"%s\",\"change\":\"%s\",\"author\":\"%s\"}",
        repo_id, artifact_id, artifact_type, change_type, author);

    strata_pub_send(pub->hub, topic, strlen(topic), payload, strlen(payload));

    return 0;
}
