#ifndef STRATA_STORE_H
#define STRATA_STORE_H

#include <stddef.h>

typedef struct strata_store strata_store;
typedef struct strata_ctx strata_ctx;

/* Artifact returned from queries */
typedef struct {
    char artifact_id[65]; /* SHA-256 hex + null */
    char repo_id[256];
    char artifact_type[64];
    char author[256];
    char created_at[32];
    unsigned char *content;
    size_t content_len;
} strata_artifact;

/* Callback for artifact listing */
typedef int (*strata_artifact_cb)(const strata_artifact *artifact, void *userdata);

/* Open/close */
strata_store *strata_store_open_sqlite(const char *path);
void          strata_store_close(strata_store *store);

/* Schema */
int strata_store_init(strata_store *store);

/* Repos */
int strata_repo_create(strata_store *store, const char *repo_id, const char *name);

/* Role management */
int strata_role_assign(strata_store *store, const char *entity_id,
                       const char *role_name, const char *repo_id);
int strata_role_revoke(strata_store *store, const char *entity_id,
                       const char *role_name, const char *repo_id);

/* Artifact CRUD (role-filtered via ctx) */
int strata_artifact_put(strata_store *store, strata_ctx *ctx,
                        const char *repo_id, const char *artifact_type,
                        const void *content, size_t content_len,
                        const char **roles, int nroles,
                        char *out_artifact_id);

int strata_artifact_get(strata_store *store, strata_ctx *ctx,
                        const char *artifact_id,
                        strata_artifact *out);

int strata_artifact_list(strata_store *store, strata_ctx *ctx,
                         const char *repo_id, const char *artifact_type,
                         strata_artifact_cb cb, void *userdata);

void strata_artifact_cleanup(strata_artifact *a);

#endif
