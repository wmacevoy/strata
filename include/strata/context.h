#ifndef STRATA_CONTEXT_H
#define STRATA_CONTEXT_H

typedef struct strata_ctx strata_ctx;
typedef struct strata_store strata_store;

strata_ctx *strata_ctx_create(const char *entity_id);
void        strata_ctx_free(strata_ctx *ctx);

const char *strata_ctx_entity_id(const strata_ctx *ctx);

/*
 * Resolve the caller's active roles for a given repo.
 * Returns count of roles, fills out_roles (caller must free each string and the array).
 */
int strata_ctx_resolve_roles(strata_ctx *ctx, strata_store *store,
                             const char *repo_id,
                             char ***out_roles, int *out_count);

void strata_ctx_free_roles(char **roles, int count);

#endif
