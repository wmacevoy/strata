#ifndef STRATA_CHANGE_H
#define STRATA_CHANGE_H

typedef struct strata_change_pub strata_change_pub;

strata_change_pub *strata_change_pub_create(const char *endpoint);
void               strata_change_pub_free(strata_change_pub *pub);

int strata_change_publish(strata_change_pub *pub,
                          const char *repo_id,
                          const char *artifact_id,
                          const char *artifact_type,
                          const char *change_type,
                          const char *author);

#endif
