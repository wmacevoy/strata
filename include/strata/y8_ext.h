/* y8_ext.h — Access y8 internals for bedrock bindings.
 *
 * y8 is opaque by design. But strata needs to add bedrock socket
 * bindings to the QuickJS context and access the SQLite handle.
 * This header mirrors y8's internal struct layout.
 *
 * MUST be kept in sync with vendor/wyatt/native/y8.c struct y8.
 */

#ifndef STRATA_Y8_EXT_H
#define STRATA_Y8_EXT_H

#include <stddef.h>
#include <sqlite3.h>

/* Forward declarations from QuickJS */
struct JSRuntime;
struct JSContext;
typedef struct JSRuntime JSRuntime;
typedef struct JSContext JSContext;

/* Mirror of struct y8 from y8.c — KEEP IN SYNC */
struct y8 {
    JSRuntime  *rt;
    JSContext  *ctx;
    sqlite3    *db;
    char       *result_buf;
    size_t      result_cap;
    char        error_buf[512];
};

#include "y8.h"

static inline JSContext *y8_get_context(y8_t *w) { return ((struct y8 *)w)->ctx; }
static inline JSRuntime *y8_get_runtime(y8_t *w) { return ((struct y8 *)w)->rt; }
static inline sqlite3   *y8_get_db(y8_t *w)      { return ((struct y8 *)w)->db; }

#endif
