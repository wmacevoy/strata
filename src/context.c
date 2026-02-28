#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include "strata/context.h"

/* Internal store access - defined in store_sqlite.c */
extern sqlite3 *strata_store_get_db(strata_store *store);

struct strata_ctx {
    char entity_id[256];
};

strata_ctx *strata_ctx_create(const char *entity_id) {
    if (!entity_id) return NULL;
    strata_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    strncpy(ctx->entity_id, entity_id, sizeof(ctx->entity_id) - 1);
    return ctx;
}

void strata_ctx_free(strata_ctx *ctx) {
    free(ctx);
}

const char *strata_ctx_entity_id(const strata_ctx *ctx) {
    return ctx ? ctx->entity_id : NULL;
}

int strata_ctx_resolve_roles(strata_ctx *ctx, strata_store *store,
                             const char *repo_id,
                             char ***out_roles, int *out_count) {
    if (!ctx || !store || !repo_id || !out_roles || !out_count) return -1;

    sqlite3 *db = strata_store_get_db(store);
    if (!db) return -1;

    const char *sql =
        "SELECT role_name FROM role_assignments "
        "WHERE entity_id = ? AND repo_id = ? "
        "AND (expires_at IS NULL OR expires_at > datetime('now'))";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, ctx->entity_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, repo_id, -1, SQLITE_STATIC);

    int capacity = 8;
    int count = 0;
    char **roles = malloc(capacity * sizeof(char *));
    if (!roles) {
        sqlite3_finalize(stmt);
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= capacity) {
            capacity *= 2;
            char **tmp = realloc(roles, capacity * sizeof(char *));
            if (!tmp) {
                for (int i = 0; i < count; i++) free(roles[i]);
                free(roles);
                sqlite3_finalize(stmt);
                return -1;
            }
            roles = tmp;
        }
        const char *r = (const char *)sqlite3_column_text(stmt, 0);
        roles[count] = strdup(r);
        count++;
    }

    sqlite3_finalize(stmt);
    *out_roles = roles;
    *out_count = count;
    return 0;
}

void strata_ctx_free_roles(char **roles, int count) {
    if (!roles) return;
    for (int i = 0; i < count; i++) free(roles[i]);
    free(roles);
}
