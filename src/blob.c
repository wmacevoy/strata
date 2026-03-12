#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sqlite3.h>
#include <sodium/crypto_hash_sha256.h>
#include "strata/blob.h"
#include "strata/aead.h"
#include "strata/context.h"

extern sqlite3 *strata_store_get_db(strata_store *store);
extern const strata_aead_key *strata_store_get_key(const strata_store *store);

/* Build AAD from sorted tags: "tag1\0tag2\0tag3\0" */
static uint8_t *build_aad(const char **tags, int ntags, size_t *aad_len) {
    size_t total = 0;
    for (int i = 0; i < ntags; i++)
        total += strlen(tags[i]) + 1;
    if (total == 0) { *aad_len = 0; return NULL; }
    uint8_t *aad = malloc(total);
    if (!aad) { *aad_len = 0; return NULL; }
    size_t pos = 0;
    for (int i = 0; i < ntags; i++) {
        size_t tlen = strlen(tags[i]);
        memcpy(aad + pos, tags[i], tlen + 1);
        pos += tlen + 1;
    }
    *aad_len = total;
    return aad;
}

/* Encrypt blob content if key is set. Caller must free returned buffer.
 * If no key, returns a copy of the input. */
static uint8_t *maybe_encrypt(strata_store *store,
                               const void *content, size_t content_len,
                               const char **tags, int ntags,
                               size_t *out_len) {
    const strata_aead_key *master = strata_store_get_key(store);
    if (!master) {
        uint8_t *copy = malloc(content_len);
        if (copy) memcpy(copy, content, content_len);
        *out_len = content_len;
        return copy;
    }

    /* Derive per-blob key from first tag (primary identity) */
    strata_aead_key derived;
    const char *key_info = (ntags > 0 && tags[0]) ? tags[0] : "blob";
    strata_aead_derive(master, key_info, &derived);

    /* Build AAD from all tags */
    size_t aad_len = 0;
    uint8_t *aad = build_aad(tags, ntags, &aad_len);

    size_t enc_len = content_len + STRATA_OVERHEAD;
    uint8_t *enc = malloc(enc_len);
    if (!enc) { free(aad); *out_len = 0; return NULL; }

    if (strata_aead_seal(&derived, content, content_len,
                          aad, aad_len, enc, &enc_len) != 0) {
        free(enc); free(aad); *out_len = 0; return NULL;
    }
    free(aad);
    *out_len = enc_len;
    return enc;
}

/* Decrypt blob content if it's sealed. Caller must free returned buffer. */
static uint8_t *maybe_decrypt(strata_store *store,
                               const void *content, size_t content_len,
                               const char *blob_id,
                               size_t *out_len) {
    if (!strata_aead_is_sealed(content, content_len)) {
        uint8_t *copy = malloc(content_len);
        if (copy) memcpy(copy, content, content_len);
        *out_len = content_len;
        return copy;
    }

    const strata_aead_key *master = strata_store_get_key(store);
    if (!master) {
        /* Sealed but no key — can't decrypt */
        *out_len = 0;
        return NULL;
    }

    /* Need to find tags for this blob to reconstruct AAD and derive key */
    sqlite3 *db = strata_store_get_db(store);
    if (!db) { *out_len = 0; return NULL; }

    char **tags = NULL;
    int ntags = 0;
    strata_blob_tags(store, blob_id, &tags, &ntags);

    /* Derive key from first tag */
    strata_aead_key derived;
    const char *key_info = (ntags > 0 && tags[0]) ? tags[0] : "blob";
    strata_aead_derive(master, key_info, &derived);

    /* Build AAD from all tags */
    size_t aad_len = 0;
    uint8_t *aad = build_aad((const char **)tags, ntags, &aad_len);

    size_t dec_len = content_len;
    uint8_t *dec = malloc(dec_len);
    if (!dec) { free(aad); strata_blob_free_tags(tags, ntags); *out_len = 0; return NULL; }

    if (strata_aead_open(&derived, content, content_len,
                          aad, aad_len, dec, &dec_len) != 0) {
        free(dec); free(aad); strata_blob_free_tags(tags, ntags);
        *out_len = 0;
        return NULL;
    }

    free(aad);
    strata_blob_free_tags(tags, ntags);
    *out_len = dec_len;
    return dec;
}

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
    unsigned char hash[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(hash, (const unsigned char *)data, len);
    for (int i = 0; i < crypto_hash_sha256_BYTES; i++)
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

    /* blob_id is hash of plaintext (preserves content-addressing) */
    char hash[65];
    sha256_hex(content, content_len, hash);

    char ts[32];
    iso8601_now(ts);

    const char *entity = strata_ctx_entity_id(ctx);

    /* Encrypt if bedrock key is set */
    size_t store_len = 0;
    uint8_t *store_content = maybe_encrypt(store, content, content_len,
                                            tags, ntags, &store_len);
    if (!store_content) return -1;

    /* Insert blob */
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT OR IGNORE INTO blobs (blob_id, content, author, created_at) "
                      "VALUES (?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(store_content);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 2, store_content, (int)store_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, entity, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, ts, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(store_content);

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

    /* Copy blob before finalize — sqlite3_column_blob is invalidated by finalize */
    void *blob_copy = malloc(blob_len);
    if (!blob_copy) { sqlite3_finalize(stmt); return -1; }
    memcpy(blob_copy, blob, blob_len);
    sqlite3_finalize(stmt);

    /* Decrypt if sealed */
    size_t dec_len = 0;
    out->content = maybe_decrypt(store, blob_copy, blob_len, out->blob_id, &dec_len);
    free(blob_copy);
    out->content_len = dec_len;

    return out->content ? 0 : -1;
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

        /* Decrypt if sealed */
        size_t dec_len = 0;
        b.content = maybe_decrypt(store, blob, blob_len, b.blob_id, &dec_len);
        b.content_len = dec_len;

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
