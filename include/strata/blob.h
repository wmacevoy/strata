#ifndef STRATA_BLOB_H
#define STRATA_BLOB_H

#include <stddef.h>

typedef struct strata_store strata_store;
typedef struct strata_ctx strata_ctx;

typedef struct {
    char blob_id[65];       /* SHA-256 hex */
    char author[256];
    char created_at[32];
    unsigned char *content;
    size_t content_len;
} strata_blob;

typedef int (*strata_blob_cb)(const strata_blob *blob, void *userdata);

/* Put a blob with tags and role permissions.
 * Returns 0 on success, fills out_blob_id with SHA-256 hex. */
int strata_blob_put(strata_store *store, strata_ctx *ctx,
                    const void *content, size_t content_len,
                    const char **tags, int ntags,
                    const char **roles, int nroles,
                    char *out_blob_id);

/* Get a blob by ID. Returns 0 if found and caller has permission. */
int strata_blob_get(strata_store *store, strata_ctx *ctx,
                    const char *blob_id, strata_blob *out);

/* Find blobs matching ALL given tags (AND logic), filtered by caller's roles.
 * If ntags == 0, returns all blobs the caller can see. */
int strata_blob_find(strata_store *store, strata_ctx *ctx,
                     const char **tags, int ntags,
                     strata_blob_cb cb, void *userdata);

/* Add/remove tags on an existing blob */
int strata_blob_tag(strata_store *store, const char *blob_id, const char *tag);
int strata_blob_untag(strata_store *store, const char *blob_id, const char *tag);

/* List tags on a blob */
int strata_blob_tags(strata_store *store, const char *blob_id,
                     char ***out_tags, int *out_count);
void strata_blob_free_tags(char **tags, int count);

void strata_blob_cleanup(strata_blob *b);

#endif
