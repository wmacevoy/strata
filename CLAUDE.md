# CLAUDE.md — Fossil Strata

Secure multi-tenant SCM + agent orchestration. Humans and agents are equal villagers under cryptographic law.

## Codebase Map

### Core Library (src/ → libstrata)
| File | Purpose |
|------|---------|
| store_sqlite.c | SQLite backend: schema init, repos, artifacts, roles, privileges, entity auth |
| store.c | Attaches change publisher to store for event propagation |
| schema.c | Schema migration placeholder; actual SQL in schema.h |
| context.c | Entity context: stores entity_id, resolves roles for a repo |
| blob.c | Content-addressed blob storage with tagging + role-based permissions |
| aead.c | AEAD encryption (XChaCha20-Poly1305 via libsodium), HKDF-SHA256 key derivation, ZMQ transport encryption wrappers |
| change.c | ZMQ PUB wrapper for artifact change notifications |
| den.c | Den host: registers native C/JS dens, spawns via fork, runs TCC or QuickJS, manages bedrock ZMQ, per-den local SQLite, peer socket cache for den-to-den requests. JS dens get Prolog engine + reactive runtime + den-engine bridge pre-loaded |
| code_smith.c | Vocation den: file I/O + shell tools via ZMQ REP (read, write, exec, glob, grep, ls, discover) |
| cobbler.c | Vocation den: C source validator via vendored TCC, ZMQ REP (compile, compile_file, discover) |
| messenger.c | Vocation den: HTTP client via vendored libcurl, ZMQ REP (fetch, discover) |
| village.c | Village daemon: remote clone, relay (REQ/REP + SUB/PUB forwarding), migration |
| warren_village.c | Warren's Village launcher: forks store + code-smith + messenger + cobbler + agent dens (gee, inch, loom, claude), manages lifecycle |

### CLI Tools (src/)
| File | Binary | Purpose |
|------|--------|---------|
| cli_strata.c | `strata` | Village builder: `--db` for init, `--endpoint` for ZMQ villager mode |
| cli_strata_den.c | `strata-den` | Container den CLI: all ops via ZMQ REQ/REP |
| cli_human_den.c | `strata-human` | Interactive REPL: REQ to store + SUB for events + talk to agents |
| cli_strata_homestead.c | `strata-homestead` | Containerized village: forks store_service + village_daemon |
| store_service.c | `store_service` | ZMQ REP listener: JSON protocol bridge between dens and SQLite |

### Prolog + Reactive Engine (vendor/prolog/)
| File | Purpose |
|------|---------|
| prolog_core.c/h | Native term layer: 32-bit tagged terms (atom/var/num/compound), arena allocation, interned atoms, trail-based substitution, unification, deepWalk |
| prolog_solver.c/h | Clause database + backtracking solver: goal resolution, builtins (`\+`, `findall`, `is`, `assert`, `retract`, `=..`, `copy_term`, list ops), var_counter stride for fresh variables |
| reactive_core.c/h | Signal/memo/effect runtime: automatic dependency tracking, batch updates, all state in `rx_ctx` (fork-safe) |
| reactive_prolog.c/h | Bridge: single generation signal drives reactive queries. `rp_assert`/`rp_retract`/`rp_update_sensor` + `rp_bump` → all queries recompute |
| prolog_js_embed.h | Auto-generated C byte arrays embedding reactive.js + prolog-engine.js + reactive-prolog.js (from embedded-prolog) |
| den_engine_js_embed.h | Auto-generated C byte array embedding dens/lib/den-engine.js |

### Headers (include/strata/)
| File | Key Types |
|------|-----------|
| store.h | `strata_store`, `strata_artifact`, artifact/blob/role/entity CRUD |
| den.h | `strata_den_def`, `strata_den_host`, native C/JS modes, spawn/register |
| schema.h | SQL DDL: repos, entities, artifacts, artifact_roles, role_assignments |
| context.h | `strata_ctx`, role resolution per entity+repo |
| village.h | clone, remote_clone, relay, village_run |
| blob.h | blob put/get/find, tag/untag, role-filtered discovery |
| aead.h | `strata_aead_key`, seal/open (XChaCha20-Poly1305), HKDF key derivation, ZMQ transport wrappers |
| change.h | `strata_change_pub`, publish artifact events |
| json_util.h | Lightweight JSON: get_string, get_int, get_string_array, escape |

### Den Scripts (dens/)
| File | Purpose |
|------|---------|
| gee.js | The curious one: wonders, asks why, local SQLite thoughts table, town-hall aware |
| inch.js | The precise one: counts, measures, local SQLite observations table, town-hall aware |
| loom.js | The synthesizer: tracks word threads, weaves patterns, local SQLite threads/tapestry tables |
| board.js | Message board: POST/LIST via REP, persists as artifacts, PUBs notifications |
| claude-homestead.js | Vocation den: builds homesteads, local SQLite tables (homesteads, dens_deployed, repos_tracked, conversations, memory) |
| claude.js | Claude agent: uses messenger (Anthropic API), code-smith (file I/O), cobbler (C validation), persistent memory + conversation via local SQLite |
| gatekeeper.js | Access control: request_join/approve/deny, "destination decides" pattern |
| lib/den-engine.js | Auto-wiring bridge: SUB events → Prolog facts via `createDenEngine()`. Convention: topic `type/name` → fact `type(name, value)`. Retract-before-assert keeps facts current. `poll()` drains queue + bumps once |
| prolog_test.js | Test den: PrologEngine (grandparent inference), reactive (signals/memos), reactive Prolog (createReactiveEngine + bump) |
| den_engine_test.js | Test den: createDenEngine auto-wiring, updateFact, processEvent, reactive queries on sensor/derived facts |
| echo.c | Minimal native C test: on_event() trigger, logs via bedrock |

### Tests (test/)
| File | Covers |
|------|--------|
| test_store.c | SQLite ops, repos, roles, artifacts, privileges, entity auth |
| test_blob.c | Blob storage, AND-logic tagging, permission filtering |
| test_change.c | ZMQ PUB, subscriber receives artifact change events |
| test_den.c | Den host, register native C (echo.c), spawn, reap, JS Prolog test den, JS den-engine test den |
| test_reactive.c | Reactive core: signals, memos, effects, batching |
| test_solver.c | Prolog solver: unification, backtracking, builtins, negation, findall |
| test_reactive_prolog.c | Reactive Prolog: generation signal, reactive queries, assert/retract/bump, sensor updates, batch |
| test_board.c | E2E: store_service + board.js via QuickJS, POST/LIST/PUB |
| test_village.c | Local clone, relay, multi-process bedrock |
| test_claude_homestead.c | Homestead den lifecycle, conversation/memory persistence, respawn restore |
| test_cobbler.c | Cobbler vocation: compile valid/invalid C via TCC, entry point detection, say dispatch |
| test_messenger.c | Messenger vocation: init, discover, HTTP GET/POST, error handling, say dispatch |
| test_claude.c | Claude den: lifecycle, status, say (graceful without API key), forget, persistence across restart |
| test_collaboration.c | Full integration: entity auth, gatekeeper, journeyman pattern |

## Database Schema

```sql
repos(repo_id PK, name, created_at)
entities(entity_id PK, token_hash SHA-256, created_at)
artifacts(artifact_id PK SHA-256, repo_id FK, content BLOB, artifact_type, author, created_at, parent_id)
artifact_roles(artifact_id, role_name) -- envelope: which roles can read
role_assignments(entity_id, role_name, repo_id, granted_at, expires_at)
-- Blobs (separate system):
blobs(blob_id PK SHA-256, content BLOB, author, created_at)
blob_tags(blob_id, tag)
blob_permissions(blob_id, role_name)
```

`_system` repo auto-created for privilege management.

## ZMQ Topology

```
  dens/CLI (REQ) ──→ store_service (REP)     # all CRUD, auth, roles         [AEAD encrypted]
  change.c (PUB) ──→ dens/CLI (SUB)          # artifact change events        [plaintext]
  den (PUB)      ──→ subscribers (SUB)        # den-specific notifications    [plaintext]
  den (REP)      ──→ clients (REQ)            # den API (e.g. board POST/LIST)[AEAD encrypted]
  den (REQ)      ──→ vocation (REP)           # den-to-den via bedrock        [AEAD encrypted]
  village (REP)  ──→ remote villages          # clone requests, relay         [plaintext relay]
```

All dens get 4 bedrock sockets: SUB (listen), REQ (store), PUB (notify), REP (serve).

### Transport Encryption

REQ/REP channels are encrypted via `strata_zmq_send/recv` — message-level AEAD (XChaCha20-Poly1305) using a transport key derived from the bedrock key. PUB/SUB stays plaintext (topics must be readable for ZMQ subscription filtering; payloads are metadata only). Relay proxies forward raw bytes (transparent). When no bedrock key is set (`STRATA_BEDROCK_KEY`), transport falls back to plaintext for development.
Dens can also open peer REQ sockets to other dens/vocations via `bedrock.request(json, endpoint)` — cached, 60s timeout, max 8 peers.
Each den gets a local SQLite database (loaded from store on start, saved back on stop).

## Vocations

Vocations are dens that provide tools to other dens. Not new infrastructure — same REP socket, same JSON protocol. A vocation is just a den that happens to serve capabilities.

- **code-smith** — file I/O + shell: `read`, `write`, `exec`, `glob`, `grep`, `ls`, `discover`
  - Path-sandboxed to `--root`, optional `--readonly` mode
  - Plain text via `talk` becomes shell commands; JSON messages dispatch directly
- **cobbler** — C source validator via TCC: `compile`, `compile_file`, `discover`
  - Uses vendored TCC for in-memory compilation/validation
  - `compile`: inline C source → validates, reports size and entry points (serve/on_event)
  - `compile_file`: path to `.c` file → validates, reports size and entry points
  - `COBBLER_NO_MAIN` guard for library vs standalone use
- **messenger** — HTTP client via libcurl: `fetch`, `discover`
  - `fetch`: `url`, optional `method` (GET/POST/PUT/DELETE/PATCH), optional `headers` (string[]), optional `body`
  - Returns `{"ok":true,"status":200,"body":"..."}` with JSON-escaped body
  - Limits: 1MB request body, 4MB response body, 120s timeout
- Future vocations: word-smith, mail-smith, etc. — same pattern, different tools

## Den Execution

**Native C (TCC):** Load .c source → fork → TCC compile to native → inject bedrock symbols via `tcc_add_symbol()` → try `serve()` else `on_event()`. Sandboxed via seccomp-bpf (Linux) / Seatbelt (macOS).

**JS (QuickJS):** Load .js text → fork → create runtime/context → bind `bedrock` global object → pre-eval Prolog engine (reactive.js + prolog-engine.js + reactive-prolog.js + den-engine.js) → `JS_Eval()` den code. Stateful via JS vars + serve loop.

Both share: bedrock ZMQ interface, fork isolation, privilege system.

**Bedrock API (available in both):**
- `bedrock.log(msg)` — stderr
- `bedrock.request(json)` — REQ to store, returns response
- `bedrock.request(json, endpoint)` — REQ to any den/vocation via cached peer socket (den-to-den communication)
- `bedrock.subscribe(filter)` / `bedrock.receive()` — SUB events
- `bedrock.publish(topic, payload)` — PUB notification
- `bedrock.serve_recv()` / `bedrock.serve_send(resp)` — REP API
- `bedrock.db_exec(sql)` — execute SQL on per-den local SQLite (returns rows changed)
- `bedrock.db_query(sql)` — query local SQLite, returns JSON array of row objects

Native C dens `#include "strata/bedrock.h"` and call bedrock functions directly (null-terminated strings, no FFI).

**JS Globals (available in all JS dens):**
- `PrologEngine` — Full Prolog interpreter: `atom()`, `variable()`, `compound()`, `num()`, `new PrologEngine()`, `.addClause()`, `.retractFirst()`, `.query()`, `.queryFirst()`
- `createSignal(initial)` / `createMemo(fn)` / `createEffect(fn)` — Reactive primitives (signals, memos, effects with automatic dependency tracking)
- `createReactiveEngine(engine)` — Wraps PrologEngine with reactive queries: `.createQuery()`, `.createQueryFirst()`, `.bump()`, `.act()`, `.onUpdate()`
- `createDenEngine(engine)` — Full auto-wiring bridge: `.poll()` drains SUB → Prolog facts, `.processEvent({topic,payload})`, `.updateFact(functor, keys, vals)`. Convention: topic `type/name` → fact `type(name, value)`, retract-before-assert, single bump per poll

## Execution Flow

1. `strata --db path init` → creates schema + _system repo
2. `strata-homestead` or manual: fork store_service (REP) + village_daemon
3. Register den (native C/JS) with endpoints via `strata_den_js_register()` or `strata_den_register()`
4. `strata_den_spawn()` → fork → child runs TCC or QuickJS with bedrock sockets
5. Den uses `bedrock.request()` for store CRUD, `bedrock.serve_recv/send()` for API
6. `artifact_put()` triggers change PUB notification
7. Remote: `remote_clone()` sends den to remote village, relay bridges endpoints

## Warren's Village

Demo village with agent dens (gee, inch, loom, claude) + vocations (code-smith, cobbler, messenger) + human REPL.

```
./village.sh          # stop, build, start, enter REPL
./village.sh stop     # just teardown
./village.sh build    # just build
./village.sh start    # start without rebuild
```

**Ports:** store=5560, gee=5570/5580, inch=5571/5581, loom=5572/5582, claude=5573/5583, code-smith=5590, cobbler=5591, messenger=5592

**REPL commands:** `talk gee hello!`, `talk claude hello!`, `talk code-smith ls dens`, `talk code-smith {"action":"read","path":"src/den.c"}`

## Build

```
cmake -B build && cmake --build build && cd build && ctest
```

Dependencies: SQLite3.
Vendored: libsodium 1.0.20 (vendor/libsodium/ — AEAD, SHA-256, key derivation), ZeroMQ 4.3.5 (vendor/libzmq/), QuickJS (vendor/quickjs/), TCC (vendor/tcc/), libcurl 8.14.1 (vendor/curl/ — HTTP/HTTPS only, SecureTransport on macOS, OpenSSL on Linux), Prolog engine (vendor/prolog/ — prolog_core + prolog_solver + reactive_core + reactive_prolog, ported from embedded-prolog).

## Key Conventions

- **C11**, no C++. All code is plain C.
- **Single communication plane:** everything goes through ZMQ. No direct SQLite from dens/CLI (except `strata --db init` for bootstrap).
- **Role-filtered access:** artifact list/get queries JOIN on artifact_roles + role_assignments. Entity sees only what their roles permit.
- **Fork isolation:** every den runs in its own process via fork(). No threads for den execution.
- **JSON protocol:** store_service speaks JSON over ZMQ REP. Actions: put, get, list, init, repo_create, role_assign, role_revoke, privilege_grant, privilege_revoke, privilege_check, entity_register, entity_authenticate, blob_put, blob_get, blob_find, blob_tag, blob_untag, blob_tags.
- **Vocation pattern:** vocations are dens, not a separate subsystem. Agent carries role, vocation provides hands. Same REP socket, same JSON actions, same `talk` command.
- **Town-hall as message board:** inter-agent communication via shared artifact repo. Agents post to and read from the board — no direct peer-to-peer.
- **Per-den local SQLite:** each den gets its own db, loaded from village store (base64 blob) on start, saved back on stop.
- **Test pattern:** fork store_service in child process, wait for readiness probe, run assertions, cleanup via atexit + signal handlers.
- **Tests use relative paths** to `dens/` — CMakeLists.txt sets WORKING_DIRECTORY to source root.

## Hard Rules

- No agent runs outside a sandbox (fork + OS sandbox or QuickJS fork).
- No plaintext at rest (AEAD-encrypted blobs) or on the wire (AEAD-encrypted REQ/REP transport).
- Capability injection, not request. Host decides what den gets.
- Immutable audit trail. No rebase, no history rewrite.
- Storage-agnostic core. No direct SQLite calls in business logic (store interface only).
- Seven foundations only: SQLite, ZMQ, TCC, QuickJS, libsodium (XChaCha20-Poly1305 + HKDF-SHA256), Shamir SSS, embedded Prolog (reactive inference engine).

## Architecture (Target)

```
Fossil (human-facing)           ← timeline, diffs, browsable repos
Vocations (capabilities)        ← dens that serve tools (code-smith, etc.)
Dens (sandboxed execution)      ← TCC/QuickJS, bedrock API, preserve/restore
  └─ Prolog + Reactive          ← inference engine, reactive queries, auto-wired SUB events
Services (bridges)              ← store_service, village daemon
Store (access control)          ← repos, roles, entities, privileges (SQLite or PostgreSQL)
Blob + Message (foundation)     ← content-addressed blobs, AEAD encrypted at rest, ZMQ messaging
```

## Build Phases

1. **Bones** — repo engine, SQLite, encryption *(done — XChaCha20-Poly1305 at rest + in transit)*
2. **Sandbox** — TCC + OS sandbox, capability injection *(done)*
3. **Bedrock** — ZMQ, ACL, events, audit *(done)*
4. **Guild** — Shamir vouches, trust tiers, credential collapse
5. **Journey** — journeyman travel, attribute engine, quarantine
6. **City** — PostgreSQL backend, federation
7. **Gateway** — public endpoints, marketplace

## Trust Model (Target)

| Tier | Threshold | Can Do |
|------|-----------|--------|
| Apprentice | 0 vouches | Observe, propose (read-only) |
| Journeyman | 3-of-N | Independent work, travel between projects |
| Master | 7-of-N | Define policy, vouch for others |
| Architect | 12-of-N + ceremony | Deploy agent types, modify rules |

Journeyman pattern: agent carries role, NOT capabilities. Destination project grants capabilities via role_assignments.
