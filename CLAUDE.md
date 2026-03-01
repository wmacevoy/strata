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
| den.c | Den host: registers WASM/JS dens, spawns via fork, runs WAMR or QuickJS, manages bedrock ZMQ |
| village.c | Village daemon: remote clone, relay (REQ/REP + SUB/PUB forwarding), migration |

### CLI Tools (src/)
| File | Binary | Purpose |
|------|--------|---------|
| cli_strata.c | `strata` | Village builder: `--db` for init, `--endpoint` for ZMQ villager mode |
| cli_strata_den.c | `strata-den` | Container den CLI: all ops via ZMQ REQ/REP |
| cli_human_den.c | `strata-human` | Interactive REPL: REQ to store + SUB for events |
| cli_strata_homestead.c | `strata-homestead` | Containerized village: forks store_service + village_daemon |
| store_service.c | `store_service` | ZMQ REP listener: JSON protocol bridge between dens and SQLite |

### Headers (include/strata/)
| File | Key Types |
|------|-----------|
| store.h | `strata_store`, `strata_artifact`, artifact/blob/role/entity CRUD |
| den.h | `strata_den_def`, `strata_den_host`, WASM/JS modes, spawn/register |
| schema.h | SQL DDL: repos, entities, artifacts, artifact_roles, role_assignments |
| context.h | `strata_ctx`, role resolution per entity+repo |
| village.h | clone, remote_clone, relay, village_run |
| blob.h | blob put/get/find, tag/untag, role-filtered discovery |
| change.h | `strata_change_pub`, publish artifact events |
| json_util.h | Lightweight JSON: get_string, get_int, get_string_array, escape |

### Den Scripts (dens/)
| File | Purpose |
|------|---------|
| board.js | Message board: POST/LIST via REP, persists as artifacts, PUBs notifications |
| claud-homestead.js | Vocation den: builds homesteads, pickle/unpickle state via blobs |
| gatekeeper.js | Access control: request_join/approve/deny, "destination decides" pattern |
| echo.wat | Minimal WASM test: on_event() trigger, logs via bedrock |

### Tests (test/)
| File | Covers |
|------|--------|
| test_store.c | SQLite ops, repos, roles, artifacts, privileges, entity auth |
| test_blob.c | Blob storage, AND-logic tagging, permission filtering |
| test_change.c | ZMQ PUB, subscriber receives artifact change events |
| test_den.c | Den host, register WASM (echo.wat), spawn, reap |
| test_board.c | E2E: store_service + board.js via QuickJS, POST/LIST/PUB |
| test_village.c | Local clone, relay, multi-process bedrock |
| test_claud_homestead.c | Homestead den lifecycle, pickle/unpickle, respawn persistence |
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
  village (REP)  ──→ remote villages          # clone requests, relay
```

All dens get 4 bedrock sockets: SUB (listen), REQ (store), PUB (notify), REP (serve).

## Den Execution

**WASM (WAMR):** Load .wasm → fork → init runtime → register native functions → try `serve()` else `on_event(ptr, len)`. Stateless by default.

**JS (QuickJS):** Load .js text → fork → create runtime/context → bind `bedrock` global object → `JS_Eval()`. Stateful via JS vars + serve loop.

Both share: bedrock ZMQ interface, fork isolation, privilege system.

**Bedrock API (available in both):**
- `bedrock.log(msg)` — stderr
- `bedrock.request(json)` — REQ to store, returns response
- `bedrock.subscribe(filter)` / `bedrock.receive()` — SUB events
- `bedrock.publish(topic, payload)` — PUB notification
- `bedrock.serve_recv()` / `bedrock.serve_send(resp)` — REP API

## Execution Flow

1. `strata --db path init` → creates schema + _system repo
2. `strata-homestead` or manual: fork store_service (REP) + village_daemon
3. Register den (WASM/JS) with endpoints via `strata_den_js_register()` or `strata_den_register()`
4. `strata_den_spawn()` → fork → child runs WAMR or QuickJS with bedrock sockets
5. Den uses `bedrock.request()` for store CRUD, `bedrock.serve_recv/send()` for API
6. `artifact_put()` triggers change PUB notification
7. Remote: `remote_clone()` sends den to remote village, relay bridges endpoints

## Build

```
cmake -B build && cmake --build build && cd build && ctest
```

Dependencies: SQLite3, ZeroMQ, WAMR (wasm-micro-runtime), OpenSSL (Linux only).
Vendored: QuickJS (vendor/quickjs/).

## Key Conventions

- **C11**, no C++. All code is plain C.
- **Single communication plane:** everything goes through ZMQ. No direct SQLite from dens/CLI (except `strata --db init` for bootstrap).
- **Role-filtered access:** artifact list/get queries JOIN on artifact_roles + role_assignments. Entity sees only what their roles permit.
- **Fork isolation:** every den runs in its own process via fork(). No threads for den execution.
- **JSON protocol:** store_service speaks JSON over ZMQ REP. Actions: put, get, list, init, repo_create, role_assign, role_revoke, privilege_grant, privilege_revoke, privilege_check, entity_register, entity_authenticate, blob_put, blob_get, blob_find, blob_tag, blob_untag, blob_tags.
- **Test pattern:** fork store_service in child process, wait for readiness probe, run assertions, cleanup via atexit + signal handlers.
- **Tests use relative paths** to `dens/` — CMakeLists.txt sets WORKING_DIRECTORY to source root.

## Hard Rules

- No agent runs outside a sandbox (WASM or QuickJS fork).
- No plaintext at rest or on the wire (goal — AEAD not yet implemented).
- Capability injection, not request. Host decides what den gets.
- Immutable audit trail. No rebase, no history rewrite.
- Storage-agnostic core. No direct SQLite calls in business logic (store interface only).
- Six foundations only: SQLite, Fossil model, WAMR, ZMQ, AEAD AES, Shamir SSS.

## Architecture (Target)

```
WAMR sandboxes (dens)           ← bidirectional fencing
ZMQ bedrock (communication)     ← ACL, encrypted, audited
AEAD + Shamir (crypto + trust)  ← role-based envelope encryption
SQLite / PostgreSQL (storage)   ← village → city migration
Fossil-model repos (audit)      ← immutable timeline
```

## Build Phases

1. **Bones** — repo engine, SQLite, encryption *(partially done)*
2. **Sandbox** — WAMR embed, capability injection *(done)*
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
