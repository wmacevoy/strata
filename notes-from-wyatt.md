# Notes from Y@ — Strata Integration Guide

The `wyatt` branch on strata has the rename done and the JS embeds
regenerated.  These notes are for whoever merges it and wires the
new modules into the den runtime.

## What's new in Y@

Six modules added to `src/`, all with Python + JS implementations,
all tested (19 suites, 500+ tests, CI on macOS/Debian/Alpine/Windows):

| Module | What it does |
|--------|-------------|
| `persist.js` | Transparent SQLite/PG persistence for Prolog engines. `persist(engine, adapter)` — hooks `on_assert`/`on_retract` callbacks. Ephemeral scopes = SQL transactions. |
| `fossilize.js` | Freeze the clause database. `assert/retract` → fail after this. Only `ephemeral/1` works. Injection proof. |
| `qjson.js` | QJSON: JSON superset. `42N` BigInt, `3.14M` BigDecimal, `3.14L` BigFloat. Comments, trailing commas, bare keys. |
| `persist-sqlite.js` | SQLite adapter for persist. Functor/arity index for filtered restore. |
| `persist-sqlcipher.js` | SQLCipher adapter (encrypted at rest). Same as sqlite + `PRAGMA key`. |
| `persist-pg.js` | PostgreSQL adapter. |

Native C:

| File | What |
|------|------|
| `native/qjson.c` | Arena-allocated QJSON. 3.5M msg/sec, zero malloc. |
| `native/wyatt.c` | Full embeddable: QuickJS + SQLite. Text in, text out. |

Engine changes (backward compatible):

- `engine.onAssert[]` / `engine.onRetract[]` — callback lists, fired from inside `assert/1`, `addClause`, `retractFirst`. Persist and reactive use these instead of monkey-patching.
- `ReactiveEngine` auto-bumps after queries that mutate facts (dirty flag + bump-after-query). Manual `bump()` still works but is usually unnecessary.

## What's already done on the `wyatt` branch

1. All 7 files renamed from `embedded-prolog` → `wyatt`
2. `gen_js_embed.py` reads from `../wyatt/src/`
3. `prolog_js_embed.h` regenerated — now includes `qjson.js`, `persist.js`, `fossilize.js` (70KB total)
4. No changes to den code yet

## What needs to happen

### 1. Wire persist into den-runtime.js

Currently `den-runtime.js` saves/restores state as a JSON snapshot
on exit/start (`_prolog_state` table).  Replace with persist's
write-through approach.

**Before** (snapshot-on-exit):
```javascript
// On startup:
var rows = bedrock.db_query("SELECT data FROM _prolog_state");
// parse JSON array, addClause each fact

// On exit:
var facts = collectRuntimeFacts(engine);
bedrock.db_exec("DELETE FROM _prolog_state");
bedrock.db_exec("INSERT INTO _prolog_state VALUES ('" + JSON.stringify(facts) + "')");
```

**After** (write-through via persist):
```javascript
// Create a bedrock-backed adapter
var adapter = {
    setup: function() {
        bedrock.db_exec(
            "CREATE TABLE IF NOT EXISTS facts " +
            "(term TEXT PRIMARY KEY, functor TEXT, arity INTEGER)");
        bedrock.db_exec(
            "CREATE INDEX IF NOT EXISTS idx_facts_pred ON facts(functor, arity)");
    },
    insert: function(key, functor, arity) {
        bedrock.db_exec(
            "INSERT OR IGNORE INTO facts VALUES ('" + key + "','" + functor + "'," + arity + ")");
    },
    remove: function(key) {
        bedrock.db_exec("DELETE FROM facts WHERE term = '" + key + "'");
    },
    all: function() {
        var rows = bedrock.db_query("SELECT term FROM facts");
        return rows.map(function(r) { return r.term; });
    },
    commit: function() {},
    close: function() {}
};

persist(engine, adapter);
```

This gives:
- Crash safety — no data loss if den crashes before clean exit
- Ephemeral = transaction — signal handlers commit atomically
- No more manual save/restore on lifecycle events

**Note**: persist hooks `on_assert`/`on_retract` callbacks (new in the
engine).  The embedded `prolog-engine.js` in `prolog_js_embed.h` already
has these callbacks (the embed was regenerated from wyatt).

### 2. Fossilize sandboxed dens

After `den.load()` (program clauses), call `fossilize(engine)` for
sandboxed dens.  This prevents any Prolog injection from modifying
the den's rules.

```javascript
// In createDen():
den.load(text);           // load program clauses (rules)
if (!opts.privileged) {
    fossilize(engine);    // freeze — only ephemeral after this
}
den.run();                // serve loop
```

Privileged dens (anthropic, code-smith) skip fossilize because they
need `assert/retract` for state management.

**Important**: `fossilize` must be called AFTER `persist` (if used).
Persist registers `on_assert`/`on_retract` listeners during setup.
Fossilize disables the assert/retract builtins but the listeners
remain active for ephemeral zone cleanup.

### 3. QJSON for bedrock transport

Replace `JSON.stringify`/`JSON.parse` in den message handling with
QJSON for BigInt/BigDecimal/BigFloat support.  The persist codec
is already wired:

```javascript
// For persist with QJSON (if dens need bignum facts):
persist(engine, adapter, null, {stringify: qjson_stringify, parse: qjson_parse});
```

For bedrock message transport, QJSON is backward compatible — valid
JSON parses identically.  The parse path tries native `JSON.parse`
first, falls back to `qjson_parse` only on exception.  Cost for
plain JSON: zero.

### 4. Native C QJSON for store_service

`qjson.c` is standalone (no QuickJS needed).  For `store_service.c`
and other C code that handles JSON messages, replace `json_util.h`
calls with `qjson.h` for bignum support.

```c
#include "qjson.h"

char arena_buf[8192];
qj_arena arena;
qj_arena_init(&arena, arena_buf, sizeof(arena_buf));

qj_val *msg = qj_parse(&arena, raw_json, len);
qj_val *action = qj_obj_get(msg, "action");
// ... dispatch ...
qj_arena_reset(&arena);  // ready for next message
```

3.5M msg/sec, zero malloc.  Arena reuse between messages.

### 5. Update gen_js_embed.py for new modules

Already done on the `wyatt` branch.  The embed now includes:

```
js_reactive_src          reactive.js
js_prolog_engine_src     prolog-engine.js (with on_assert/on_retract)
js_parser_src            parser.js
js_reactive_prolog_src   reactive-prolog.js (with auto-bump)
js_qjson_src             qjson.js
js_persist_src           persist.js
js_fossilize_src         fossilize.js
js_load_string_src       loadString (inline)
```

After eval'ing these in QuickJS, all functions are globals.  Dens
get `persist()`, `fossilize()`, `qjson_parse()`, `qjson_stringify()`
for free.

### 6. Adapter interface

The persist adapter is 6 methods:

```
setup()                      create table + index
insert(key, functor, arity)  upsert fact
remove(key)                  delete by key
all(predicates)              filtered or full restore
commit()                     commit transaction
close()                      release
```

For strata, the adapter wraps `bedrock.db_exec`/`bedrock.db_query`.
For a future AEAD-at-the-value adapter, encrypt `key` before insert,
decrypt after select.  The persist module doesn't care what the
adapter does internally.

## Migration order

1. Merge the `wyatt` branch (rename + regenerated embeds)
2. Wire the bedrock adapter into `den-runtime.js`
3. Add `fossilize()` to sandboxed den startup
4. Replace `_prolog_state` snapshot code with persist
5. (Optional) Add QJSON to bedrock transport
6. (Optional) Add `qjson.c` to store_service

Steps 1-2 can be done together.  Step 3 is independent.  Step 4
removes old code.  Steps 5-6 are future optimization.

## Testing

Run strata's existing test suite after each step.  The engine changes
(on_assert/on_retract, auto-bump) are backward compatible — old code
that calls `bump()` manually still works (redundant but harmless).

The `test_den.c` runtime_test (41 tests) covers state persistence
round-trip.  After wiring persist, those tests should still pass
with write-through instead of snapshot-on-exit.
