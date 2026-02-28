#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sqlite3.h>
#include "strata/store.h"
#include "strata/context.h"
#include "strata/schema.h"
#include "strata/change.h"

/* SHA-256 - use CommonCrypto on macOS, or a minimal impl */
#include <CommonCrypto/CommonDigest.h>

struct strata_store {
    sqlite3 *db;
    strata_change_pub *change_pub;
};

/* Expose db handle for context.c */
sqlite3 *strata_store_get_db(strata_store *store) {
    return store ? store->db : NULL;
}

static void sha256_hex(const void *data, size_t len, char out[65]) {
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data, (CC_LONG)len, hash);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
        sprintf(out + i * 2, "%02x", hash[i]);
    out[64] = '\0';
}

static void iso8601_now(char buf[32]) {
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    strftime(buf, 32, "%Y-%m-%dT%H:%M:%SZ", tm);
}

strata_store *strata_store_open_sqlite(const char *path) {
    strata_store *store = calloc(1, sizeof(*store));
    if (!store) return NULL;

    if (sqlite3_open(path, &store->db) != SQLITE_OK) {
        free(store);
        return NULL;
    }

    sqlite3_exec(store->db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(store->db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);

    return store;
}

void strata_store_close(strata_store *store) {
    if (!store) return;
    if (store->change_pub) strata_change_pub_free(store->change_pub);
    if (store->db) sqlite3_close(store->db);
    free(store);
}

int strata_store_init(strata_store *store) {
    if (!store || !store->db) return -1;

    char *err = NULL;
    if (sqlite3_exec(store->db, STRATA_SCHEMA_SQL, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "schema error: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
    if (sqlite3_exec(store->db, STRATA_INDEX_SQL, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "index error: %s\n", err);
        sqlite3_free(err);
        return -1;
    }

    /* Auto-create _system repo for privilege management */
    strata_repo_create(store, "_system", "System privileges");
    /* Ignore error — repo may already exist */

    return 0;
}

int strata_repo_create(strata_store *store, const char *repo_id, const char *name) {
    if (!store || !repo_id || !name) return -1;

    const char *sql = "INSERT INTO repos (repo_id, name, created_at) VALUES (?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    char ts[32];
    iso8601_now(ts);

    sqlite3_bind_text(stmt, 1, repo_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, ts, -1, SQLITE_STATIC);

    int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int strata_role_assign(strata_store *store, const char *entity_id,
                       const char *role_name, const char *repo_id) {
    if (!store || !entity_id || !role_name || !repo_id) return -1;

    const char *sql =
        "INSERT OR REPLACE INTO role_assignments "
        "(entity_id, role_name, repo_id, granted_at) VALUES (?, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    char ts[32];
    iso8601_now(ts);

    sqlite3_bind_text(stmt, 1, entity_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, role_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, repo_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, ts, -1, SQLITE_STATIC);

    int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int strata_role_revoke(strata_store *store, const char *entity_id,
                       const char *role_name, const char *repo_id) {
    if (!store || !entity_id || !role_name || !repo_id) return -1;

    const char *sql =
        "DELETE FROM role_assignments "
        "WHERE entity_id = ? AND role_name = ? AND repo_id = ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, entity_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, role_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, repo_id, -1, SQLITE_STATIC);

    int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

/* Attach a ZMQ change publisher to this store */
void strata_store_set_change_pub(strata_store *store, strata_change_pub *pub) {
    if (store) store->change_pub = pub;
}

int strata_artifact_put(strata_store *store, strata_ctx *ctx,
                        const char *repo_id, const char *artifact_type,
                        const void *content, size_t content_len,
                        const char **roles, int nroles,
                        char *out_artifact_id) {
    if (!store || !ctx || !repo_id || !artifact_type || !content || !roles || nroles <= 0)
        return -1;

    char hash[65];
    sha256_hex(content, content_len, hash);

    char ts[32];
    iso8601_now(ts);

    const char *entity = strata_ctx_entity_id(ctx);

    /* Insert artifact */
    const char *sql =
        "INSERT INTO artifacts (artifact_id, repo_id, content, artifact_type, author, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, repo_id, -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 3, content, (int)content_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, artifact_type, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, entity, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, ts, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);

    /* Insert role grants for this artifact */
    const char *role_sql =
        "INSERT INTO artifact_roles (artifact_id, role_name) VALUES (?, ?)";
    for (int i = 0; i < nroles; i++) {
        if (sqlite3_prepare_v2(store->db, role_sql, -1, &stmt, NULL) != SQLITE_OK)
            return -1;
        sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, roles[i], -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return -1;
        }
        sqlite3_finalize(stmt);
    }

    if (out_artifact_id)
        memcpy(out_artifact_id, hash, 65);

    /* Publish change event */
    if (store->change_pub) {
        strata_change_publish(store->change_pub, repo_id, hash,
                              artifact_type, "create", entity);
    }

    return 0;
}

int strata_artifact_get(strata_store *store, strata_ctx *ctx,
                        const char *artifact_id, strata_artifact *out) {
    if (!store || !ctx || !artifact_id || !out) return -1;

    /*
     * Role-filtered query: only return the artifact if the caller
     * has at least one role that matches artifact_roles.
     * We resolve roles from the artifact's repo.
     */
    const char *sql =
        "SELECT DISTINCT a.artifact_id, a.repo_id, a.artifact_type, "
        "  a.author, a.created_at, a.content "
        "FROM artifacts a "
        "JOIN artifact_roles ar ON a.artifact_id = ar.artifact_id "
        "JOIN role_assignments ra ON ar.role_name = ra.role_name "
        "  AND ra.repo_id = a.repo_id "
        "WHERE a.artifact_id = ? AND ra.entity_id = ? "
        "AND (ra.expires_at IS NULL OR ra.expires_at > datetime('now'))";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, artifact_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, strata_ctx_entity_id(ctx), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;  /* not found or no access */
    }

    memset(out, 0, sizeof(*out));
    strncpy(out->artifact_id, (const char *)sqlite3_column_text(stmt, 0), 64);
    strncpy(out->repo_id, (const char *)sqlite3_column_text(stmt, 1), 255);
    strncpy(out->artifact_type, (const char *)sqlite3_column_text(stmt, 2), 63);
    strncpy(out->author, (const char *)sqlite3_column_text(stmt, 3), 255);
    strncpy(out->created_at, (const char *)sqlite3_column_text(stmt, 4), 31);

    const void *blob = sqlite3_column_blob(stmt, 5);
    int blob_len = sqlite3_column_bytes(stmt, 5);
    out->content = malloc(blob_len);
    if (out->content) {
        memcpy(out->content, blob, blob_len);
        out->content_len = blob_len;
    }

    sqlite3_finalize(stmt);
    return 0;
}

int strata_artifact_list(strata_store *store, strata_ctx *ctx,
                         const char *repo_id, const char *artifact_type,
                         strata_artifact_cb cb, void *userdata) {
    if (!store || !ctx || !repo_id || !cb) return -1;

    const char *sql_with_type =
        "SELECT DISTINCT a.artifact_id, a.repo_id, a.artifact_type, "
        "  a.author, a.created_at, a.content "
        "FROM artifacts a "
        "JOIN artifact_roles ar ON a.artifact_id = ar.artifact_id "
        "JOIN role_assignments ra ON ar.role_name = ra.role_name "
        "  AND ra.repo_id = a.repo_id "
        "WHERE a.repo_id = ? AND ra.entity_id = ? AND a.artifact_type = ? "
        "AND (ra.expires_at IS NULL OR ra.expires_at > datetime('now')) "
        "ORDER BY a.created_at";

    const char *sql_all =
        "SELECT DISTINCT a.artifact_id, a.repo_id, a.artifact_type, "
        "  a.author, a.created_at, a.content "
        "FROM artifacts a "
        "JOIN artifact_roles ar ON a.artifact_id = ar.artifact_id "
        "JOIN role_assignments ra ON ar.role_name = ra.role_name "
        "  AND ra.repo_id = a.repo_id "
        "WHERE a.repo_id = ? AND ra.entity_id = ? "
        "AND (ra.expires_at IS NULL OR ra.expires_at > datetime('now')) "
        "ORDER BY a.created_at";

    const char *sql = artifact_type ? sql_with_type : sql_all;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, repo_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, strata_ctx_entity_id(ctx), -1, SQLITE_STATIC);
    if (artifact_type)
        sqlite3_bind_text(stmt, 3, artifact_type, -1, SQLITE_STATIC);

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        strata_artifact a;
        memset(&a, 0, sizeof(a));
        strncpy(a.artifact_id, (const char *)sqlite3_column_text(stmt, 0), 64);
        strncpy(a.repo_id, (const char *)sqlite3_column_text(stmt, 1), 255);
        strncpy(a.artifact_type, (const char *)sqlite3_column_text(stmt, 2), 63);
        strncpy(a.author, (const char *)sqlite3_column_text(stmt, 3), 255);
        strncpy(a.created_at, (const char *)sqlite3_column_text(stmt, 4), 31);

        const void *blob = sqlite3_column_blob(stmt, 5);
        int blob_len = sqlite3_column_bytes(stmt, 5);
        a.content = malloc(blob_len);
        if (a.content) {
            memcpy(a.content, blob, blob_len);
            a.content_len = blob_len;
        }

        int keep_going = cb(&a, userdata);
        free(a.content);
        if (!keep_going) break;
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

void strata_artifact_cleanup(strata_artifact *a) {
    if (a && a->content) {
        free(a->content);
        a->content = NULL;
        a->content_len = 0;
    }
}

int strata_has_privilege(strata_store *store, const char *entity_id,
                         const char *privilege) {
    if (!store || !entity_id || !privilege) return 0;

    const char *sql =
        "SELECT 1 FROM role_assignments "
        "WHERE entity_id = ? AND role_name = ? AND repo_id = '_system' "
        "AND (expires_at IS NULL OR expires_at > datetime('now')) "
        "LIMIT 1";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_text(stmt, 1, entity_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, privilege, -1, SQLITE_STATIC);

    int has = (sqlite3_step(stmt) == SQLITE_ROW) ? 1 : 0;
    sqlite3_finalize(stmt);
    return has;
}
