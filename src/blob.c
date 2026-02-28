#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sqlite3.h>
#include <CommonCrypto/CommonDigest.h>
#include "strata/blob.h"
#include "strata/context.h"

extern sqlite3 *strata_store_get_db(strata_store *store);

static const char *BLOB_SCHEMA =
    "CREATE TABLE IF NOT EXISTS blobs ("
    "  blob_id    TEXT PRIMARY KEY,"
    "  content    BLOB NOT NULL,"
    "  author     TEXT NOT NULL,"
    "  created_at TEXT NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS blob_tags ("
    "  blob_id TEXT NOT NULL REFERENCES blobs(blob_id),"
    "  tag     TEXT NOT NULL,"
    "  PRIMARY KEY (blob_id, tag)"
    ");"
    "CREATE TABLE IF NOT EXISTS blob_permissions ("
    "  blob_id   TEXT NOT NULL REFERENCES blobs(blob_id),"
    "  role_name TEXT NOT NULL,"
    "  PRIMARY KEY (blob_id, role_name)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_blob_tags_tag ON blob_tags(tag);"
    "CREATE INDEX IF NOT EXISTS idx_blob_perms_role ON blob_permissions(role_name);";

/* Auto-init blob tables on first use */
static int blob_ensure_schema(sqlite3 *db) {
    static int initialized = 0;
    if (initialized) return 0;
    char *err = NULL;
    if (sqlite3_exec(db, BLOB_SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "blob schema: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
    /* Ensure "global" repo exists for blob role assignments */
    const char *sql = "INSERT OR IGNORE INTO repos (repo_id, name, created_at) "
                      "VALUES ('global', 'Global', datetime('now'))";
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    initialized = 1;
    return 0;
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

int strata_blob_put(strata_store *store, strata_ctx *ctx,
                    const void *content, size_t content_len,
                    const char **tags, int ntags,
                    const char **roles, int nroles,
                    char *out_blob_id) {
    if (!store || !ctx || !content || !roles || nroles <= 0) return -1;

    sqlite3 *db = strata_store_get_db(store);
    if (!db || blob_ensure_schema(db) != 0) return -1;

    char hash[65];
    sha256_hex(content, content_len, hash);

    char ts[32];
    iso8601_now(ts);

    const char *entity = strata_ctx_entity_id(ctx);

    /* Insert blob */
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT OR IGNORE INTO blobs (blob_id, content, author, created_at) "
                      "VALUES (?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 2, content, (int)content_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, entity, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, ts, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* Insert tags */
    for (int i = 0; i < ntags; i++) {
        sql = "INSERT OR IGNORE INTO blob_tags (blob_id, tag) VALUES (?, ?)";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, tags[i], -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    /* Insert permissions */
    for (int i = 0; i < nroles; i++) {
        sql = "INSERT OR IGNORE INTO blob_permissions (blob_id, role_name) VALUES (?, ?)";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, roles[i], -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    if (out_blob_id) memcpy(out_blob_id, hash, 65);
    return 0;
}

int strata_blob_get(strata_store *store, strata_ctx *ctx,
                    const char *blob_id, strata_blob *out) {
    if (!store || !ctx || !blob_id || !out) return -1;

    sqlite3 *db = strata_store_get_db(store);
    if (!db || blob_ensure_schema(db) != 0) return -1;

    const char *sql =
        "SELECT DISTINCT b.blob_id, b.author, b.created_at, b.content "
        "FROM blobs b "
        "JOIN blob_permissions bp ON b.blob_id = bp.blob_id "
        "JOIN role_assignments ra ON bp.role_name = ra.role_name "
        "WHERE b.blob_id = ? AND ra.entity_id = ? "
        "AND (ra.expires_at IS NULL OR ra.expires_at > datetime('now'))";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, blob_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, strata_ctx_entity_id(ctx), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    strncpy(out->blob_id, (const char *)sqlite3_column_text(stmt, 0), 64);
    strncpy(out->author, (const char *)sqlite3_column_text(stmt, 1), 255);
    strncpy(out->created_at, (const char *)sqlite3_column_text(stmt, 2), 31);

    const void *blob = sqlite3_column_blob(stmt, 3);
    int blob_len = sqlite3_column_bytes(stmt, 3);
    out->content = malloc(blob_len);
    if (out->content) {
        memcpy(out->content, blob, blob_len);
        out->content_len = blob_len;
    }

    sqlite3_finalize(stmt);
    return 0;
}

int strata_blob_find(strata_store *store, strata_ctx *ctx,
                     const char **tags, int ntags,
                     strata_blob_cb cb, void *userdata) {
    if (!store || !ctx || !cb) return -1;

    sqlite3 *db = strata_store_get_db(store);
    if (!db || blob_ensure_schema(db) != 0) return -1;

    /* Build query dynamically based on number of tags */
    char sql[4096];
    int pos;

    if (ntags > 0) {
        pos = snprintf(sql, sizeof(sql),
            "SELECT DISTINCT b.blob_id, b.author, b.created_at, b.content "
            "FROM blobs b "
            "JOIN blob_permissions bp ON b.blob_id = bp.blob_id "
            "JOIN role_assignments ra ON bp.role_name = ra.role_name "
            "JOIN blob_tags bt ON b.blob_id = bt.blob_id "
            "WHERE ra.entity_id = ? "
            "AND (ra.expires_at IS NULL OR ra.expires_at > datetime('now')) "
            "AND bt.tag IN (");
        for (int i = 0; i < ntags; i++)
            pos += snprintf(sql + pos, sizeof(sql) - pos, "%s?", i > 0 ? "," : "");
        pos += snprintf(sql + pos, sizeof(sql) - pos,
            ") GROUP BY b.blob_id HAVING COUNT(DISTINCT bt.tag) = ? "
            "ORDER BY b.created_at");
    } else {
        snprintf(sql, sizeof(sql),
            "SELECT DISTINCT b.blob_id, b.author, b.created_at, b.content "
            "FROM blobs b "
            "JOIN blob_permissions bp ON b.blob_id = bp.blob_id "
            "JOIN role_assignments ra ON bp.role_name = ra.role_name "
            "WHERE ra.entity_id = ? "
            "AND (ra.expires_at IS NULL OR ra.expires_at > datetime('now')) "
            "ORDER BY b.created_at");
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "blob_find prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    int bind = 1;
    sqlite3_bind_text(stmt, bind++, strata_ctx_entity_id(ctx), -1, SQLITE_STATIC);
    for (int i = 0; i < ntags; i++)
        sqlite3_bind_text(stmt, bind++, tags[i], -1, SQLITE_STATIC);
    if (ntags > 0)
        sqlite3_bind_int(stmt, bind++, ntags);

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        strata_blob b;
        memset(&b, 0, sizeof(b));
        strncpy(b.blob_id, (const char *)sqlite3_column_text(stmt, 0), 64);
        strncpy(b.author, (const char *)sqlite3_column_text(stmt, 1), 255);
        strncpy(b.created_at, (const char *)sqlite3_column_text(stmt, 2), 31);

        const void *blob = sqlite3_column_blob(stmt, 3);
        int blob_len = sqlite3_column_bytes(stmt, 3);
        b.content = malloc(blob_len);
        if (b.content) {
            memcpy(b.content, blob, blob_len);
            b.content_len = blob_len;
        }

        int keep = cb(&b, userdata);
        free(b.content);
        if (!keep) break;
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

int strata_blob_tag(strata_store *store, const char *blob_id, const char *tag) {
    if (!store || !blob_id || !tag) return -1;
    sqlite3 *db = strata_store_get_db(store);
    if (!db) return -1;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT OR IGNORE INTO blob_tags (blob_id, tag) VALUES (?, ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, blob_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, tag, -1, SQLITE_STATIC);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int strata_blob_untag(strata_store *store, const char *blob_id, const char *tag) {
    if (!store || !blob_id || !tag) return -1;
    sqlite3 *db = strata_store_get_db(store);
    if (!db) return -1;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "DELETE FROM blob_tags WHERE blob_id = ? AND tag = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, blob_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, tag, -1, SQLITE_STATIC);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int strata_blob_tags(strata_store *store, const char *blob_id,
                     char ***out_tags, int *out_count) {
    if (!store || !blob_id || !out_tags || !out_count) return -1;
    sqlite3 *db = strata_store_get_db(store);
    if (!db) return -1;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT tag FROM blob_tags WHERE blob_id = ? ORDER BY tag";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, blob_id, -1, SQLITE_STATIC);

    int cap = 8, count = 0;
    char **tags = malloc(cap * sizeof(char *));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            tags = realloc(tags, cap * sizeof(char *));
        }
        tags[count++] = strdup((const char *)sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);

    *out_tags = tags;
    *out_count = count;
    return 0;
}

void strata_blob_free_tags(char **tags, int count) {
    if (!tags) return;
    for (int i = 0; i < count; i++) free(tags[i]);
    free(tags);
}

void strata_blob_cleanup(strata_blob *b) {
    if (b && b->content) {
        free(b->content);
        b->content = NULL;
        b->content_len = 0;
    }
}
