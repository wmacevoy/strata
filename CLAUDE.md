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
| change.c | ZMQ PUB wrapper for artifact change notifications |
| den.c | Den host: registers native C/JS dens, spawns via fork, runs TCC or QuickJS, manages bedrock ZMQ, per-den local SQLite, peer socket cache for den-to-den requests |
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

### Headers (include/strata/)
| File | Key Types |
|------|-----------|
| store.h | `strata_store`, `strata_artifact`, artifact/blob/role/entity CRUD |
| den.h | `strata_den_def`, `strata_den_host`, native C/JS modes, spawn/register |
| schema.h | SQL DDL: repos, entities, artifacts, artifact_roles, role_assignments |
| context.h | `strata_ctx`, role resolution per entity+repo |
| village.h | clone, remote_clone, relay, village_run |
| blob.h | blob put/get/find, tag/untag, role-filtered discovery |
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
| echo.c | Minimal native C test: on_event() trigger, logs via bedrock |

### Tests (test/)
| File | Covers |
|------|--------|
| test_store.c | SQLite ops, repos, roles, artifacts, privileges, entity auth |
| test_blob.c | Blob storage, AND-logic tagging, permission filtering |
| test_change.c | ZMQ PUB, subscriber receives artifact change events |
| test_den.c | Den host, register native C (echo.c), spawn, reap |
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
  dens/CLI (REQ) ──→ store_service (REP)     # all CRUD, auth, roles
  change.c (PUB) ──→ dens/CLI (SUB)          # artifact change events
  den (PUB)      ──→ subscribers (SUB)        # den-specific notifications
  den (REP)      ──→ clients (REQ)            # den API (e.g. board POST/LIST)
  den (REQ)      ──→ vocation (REP)           # den-to-den via bedrock.request(json, endpoint)
  village (REP)  ──→ remote villages          # clone requests, relay
```

All dens get 4 bedrock sockets: SUB (listen), REQ (store), PUB (notify), REP (serve).
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

**JS (QuickJS):** Load .js text → fork → create runtime/context → bind `bedrock` global object → `JS_Eval()`. Stateful via JS vars + serve loop.

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

Dependencies: SQLite3, ZeroMQ, OpenSSL (Linux only).
Vendored: QuickJS (vendor/quickjs/), TCC (vendor/tcc/), libcurl 8.14.1 (vendor/curl/ — HTTP/HTTPS only, SecureTransport on macOS, OpenSSL on Linux).

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
- No plaintext at rest or on the wire (goal — AEAD not yet implemented).
- Capability injection, not request. Host decides what den gets.
- Immutable audit trail. No rebase, no history rewrite.
- Storage-agnostic core. No direct SQLite calls in business logic (store interface only).
- Six foundations only: SQLite, ZMQ, TCC, QuickJS, AEAD AES, Shamir SSS.

## Architecture (Target)

```
Fossil (human-facing)           ← timeline, diffs, browsable repos
Vocations (capabilities)        ← dens that serve tools (code-smith, etc.)
Dens (sandboxed execution)      ← TCC/QuickJS, bedrock API, preserve/restore
Services (bridges)              ← store_service, village daemon
Store (access control)          ← repos, roles, entities, privileges (SQLite or PostgreSQL)
Blob + Message (foundation)     ← content-addressed blobs, AEAD encrypted at rest, ZMQ messaging
```

## Build Phases

1. **Bones** — repo engine, SQLite, encryption *(partially done)*
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
