# Architecture — Fossil Strata

Strata is a secure multi-tenant SCM and agent orchestration platform. The foundation is blob storage and ZMQ messaging — not Fossil. Fossil is a human-facing layer near the top, built on blobs and artifacts. Humans and agents are equal villagers: same trust mechanics, same communication plane, same access control.

## The Stack

```
Layer 6: Fossil         — human-facing timeline, history, diffs (built on blobs)
Layer 5: Vocations      — dens that serve capabilities (code-smith, claude-homestead)
Layer 4: Dens           — sandboxed execution, bedrock API, preserve/restore
Layer 3: Services       — store_service JSON bridge, village daemon
Layer 2: Store          — repos, roles, entities, privileges
Layer 1: Blob + Message — content-addressed blobs, ZMQ messaging
Layer 0: Foundation     — SQLite, ZMQ, QuickJS, TCC
```

---

## Layer 0 — Foundation

Six components. Everything else is composed from these.

| Component | Role |
|-----------|------|
| SQLite / PostgreSQL | Village store engine. SQLite for single-machine villages, PostgreSQL for city-scale. Same interface, swappable. |
| SQLite | Den local storage. Permanent choice — every den gets its own SQLite db. Not swappable. |
| ZeroMQ | Communication. Brokerless messaging. REQ/REP, PUB/SUB. The sole communication plane. |
| libsodium | Encryption. XChaCha20-Poly1305 AEAD for blobs at rest and ZMQ transport in transit. HKDF-SHA256 for key derivation. SHA-256 for content addressing. |
| QuickJS | JS den runtime. Stateful serve loops. First-class alongside native C. |
| TCC | Native C den runtime. Vendored ~100KB compiler. Compiles C to native code, runs in fork-isolated sandbox. |

Planned but not yet implemented:

| Component | Role | Status |
|-----------|------|--------|
| Shamir SSS | Trust governance. M-of-N vouches, credential reconstruction. | Planned |

**Two levels of SQLite:** The village store may be SQLite or PostgreSQL — that's a deployment choice. Den local databases are always SQLite — that's a permanent architectural choice. Dens are lightweight, portable, fork-isolated; SQLite is the right fit. The village store handles concurrency, cross-repo queries, and federation; PostgreSQL enables city-scale.

---

## Layer 1 — Blob + Message Store

The foundation of all persistence and communication. Everything above is built on blobs and ZMQ messages.

### Blobs

Content-addressed storage. A blob is identified by the SHA-256 hash of its content.

```sql
blobs(blob_id TEXT PK, content BLOB, author TEXT, created_at TEXT)
blob_tags(blob_id TEXT, tag TEXT)           -- many tags per blob
blob_permissions(blob_id TEXT, role_name TEXT) -- who can read
```

**Any blob can be a preserved fossil** — den state, den source code, a restore plan, a human's commit. The blob layer doesn't know or care what's inside.

**AEAD encrypted at rest.** Every blob's content is sealed with XChaCha20-Poly1305 before storage. The bedrock key (set via `STRATA_BEDROCK_KEY` or `STRATA_BEDROCK_KEY_FILE`) is the master; per-blob keys are derived via HKDF-SHA256 with the blob's tags as context. Wire format: `"AE02" (4) || nonce (24) || ciphertext+tag (N+16)` — 44 bytes overhead. Role-keyed envelopes determine who can decrypt: an entity without the right role sees ciphertext, not "access denied." This is foundational — encryption lives in the blob layer, not above it.

**Tag search uses AND logic.** Searching for tags `["a", "b"]` finds blobs tagged with both `a` AND `b`:
```sql
WHERE bt.tag IN (?, ?) GROUP BY b.blob_id HAVING COUNT(DISTINCT bt.tag) = 2
```

**Access is role-filtered.** Blob queries JOIN on `blob_permissions` + `role_assignments`. You see only what your roles permit.

Key functions (`blob.c`): `strata_blob_put`, `strata_blob_get`, `strata_blob_find`, `strata_blob_tag`, `strata_blob_untag`.

### ZMQ Messaging

Four socket patterns, used everywhere:

| Pattern | Direction | Purpose | Encryption |
|---------|-----------|---------|------------|
| REQ/REP | REQ connects → REP binds | Synchronous request/response (store queries, den API) | AEAD encrypted |
| PUB/SUB | PUB binds → SUB connects | Event broadcast (artifact changes, den notifications) | Plaintext |

Every entity communicates through ZMQ. No direct database access from dens or CLIs (except `strata --db init` for bootstrap).

**Transport encryption** is message-level AEAD, not ZMQ CURVE. Each REQ/REP frame is sealed with XChaCha20-Poly1305 using a transport key derived from the bedrock key (`HKDF-SHA256(bedrock, "zmq-transport")`). The `strata_zmq_send/recv` wrappers handle this transparently. PUB/SUB stays plaintext because ZMQ subscription filtering requires readable topic prefixes; payloads on PUB/SUB are metadata (repo IDs, artifact types), not sensitive content. When no bedrock key is configured, transport falls back to plaintext.

### To extend Layer 1

- New blob operations: add to `blob.c` + `store.h`, wire into `store_service.c` JSON dispatch
- New message patterns: add ZMQ socket to relevant component, document endpoint

---

## Layer 2 — Store

The role-filtered data repository. Repos, artifacts, entities, and access control. The store interface is backend-agnostic — currently implemented on SQLite (`store_sqlite.c`), designed to support PostgreSQL for city-scale deployment without changing any code above this layer.

### Schema

```sql
repos(repo_id TEXT PK, name TEXT, created_at TEXT)
entities(entity_id TEXT PK, token_hash TEXT, created_at TEXT)
artifacts(artifact_id TEXT PK, repo_id TEXT, content BLOB,
          artifact_type TEXT, author TEXT, created_at TEXT, parent_id TEXT)
artifact_roles(artifact_id TEXT, role_name TEXT)  -- envelope: who can read
role_assignments(entity_id TEXT, role_name TEXT, repo_id TEXT,
                 granted_at TEXT, expires_at TEXT)
```

### Access Model

Every artifact query JOINs on `artifact_roles` + `role_assignments`. An entity sees only artifacts whose roles overlap with their assigned roles for that repo. Time-based expiry: `expires_at IS NULL OR expires_at > datetime('now')`.

### The `_system` Repo

Auto-created on init. Privileges are stored as role_assignments in `_system`:
- `parent` — can this entity spawn dens?
- `vocation` — can this entity have PUB/SUB endpoints?
- Custom privileges — any string

Check via `strata_has_privilege(store, entity, privilege)`.

### Entity Authentication

Entities register with a token. Token is SHA-256 hashed at rest. Authentication = hash the presented token and compare.

### Change Notifications

`artifact_put()` triggers a ZMQ PUB message:
```
Topic: "change/{repo_id}/{artifact_type}"
Payload: {"repo_id":"...", "artifact_id":"...", "change":"create", "author":"..."}
```

Key files: `store_sqlite.c`, `context.c`, `schema.h`, `change.c`.

### To extend Layer 2

- New entity fields: modify schema in `schema.h`, update `store_sqlite.c`
- New access patterns: add query functions to `store_sqlite.c`, expose via store.h

---

## Layer 3 — Services

Processes that bridge layers. Dens and CLIs talk to services; services talk to the store.

### store_service

ZMQ REP listener. Receives JSON, dispatches to store, returns JSON. This is how all dens and CLIs access the database.

**Protocol** — every request has an `action` field:

| Action | Purpose |
|--------|---------|
| `put` | Create artifact (with role envelope) |
| `get` | Retrieve artifact (role-filtered) |
| `list` | List artifacts (role-filtered) |
| `repo_create` | Create a new repo |
| `role_assign` / `role_revoke` | Grant/revoke roles |
| `privilege_grant` / `privilege_revoke` / `privilege_check` | System privileges |
| `entity_register` / `entity_authenticate` | Identity management |
| `blob_put` / `blob_get` / `blob_find` | Blob operations |
| `blob_tag` / `blob_untag` / `blob_tags` | Blob tag management |
| `init` | Readiness probe (returns `{"ok":true}`) |

Key file: `store_service.c`.

### Village Daemon

Listens for remote clone requests. Receives a den definition (source + event), spawns it locally, returns endpoints. Enables den migration between villages.

**Remote clone frame protocol** (3-frame ZMQ multipart):
1. Header JSON: `{"action":"clone", "den_name":"...", "mode":"js", "origin_req":"..."}`
2. Den source: JS text or C source
3. Event JSON: initial payload

The relay system bridges sockets between villages: REQ/REP proxy for store access, SUB/PUB proxy for event forwarding.

Key file: `village.c`.

### To extend Layer 3

- New store_service action: add case to `handle_request()` in `store_service.c`
- New service type: follow `store_service.c` or `code_smith.c` pattern (ZMQ REP loop + JSON dispatch + signal handling)

---

## Layer 4 — Dens

Sandboxed execution units. Every den runs in its own process (fork isolation) with a standardized communication interface.

### Bedrock API

Every den — native C or JS — gets the same 9 functions:

| Function | Socket | Purpose |
|----------|--------|---------|
| `bedrock.log(msg)` | stderr | Logging |
| `bedrock.request(json)` | REQ | Send request to store_service, get response |
| `bedrock.subscribe(filter)` | SUB | Subscribe to event topics |
| `bedrock.receive()` | SUB | Receive subscribed event (topic + payload) |
| `bedrock.publish(topic, payload)` | PUB | Broadcast notification |
| `bedrock.serve_recv()` | REP | Wait for incoming API request |
| `bedrock.serve_send(response)` | REP | Send API response |
| `bedrock.db_exec(sql)` | local SQLite | Execute DDL/DML, returns rows changed |
| `bedrock.db_query(sql)` | local SQLite | Query, returns JSON array of row objects |

### Two Runtimes

**JS (QuickJS):** Load .js → fork → bind `bedrock` global → `JS_Eval()`. Script runs its own serve loop. Stateful via JS vars + local SQLite.

**Native C (TCC):** Load .c source → fork → TCC compile to native → inject bedrock symbols via `tcc_add_symbol()` → try `serve()` else `on_event()`. Sandboxed via seccomp-bpf (Linux) / Seatbelt (macOS). Bedrock functions use null-terminated strings (not ptr/len pairs).

Both share identical bedrock interface, fork isolation, and privilege system.

### Den Registration and Spawn

```c
// Register: load source, store definition
strata_den_js_register(host, name, js_path, trigger, store_ep, pub_ep, rep_ep);

// Spawn: privilege check → fork → child runs runtime
pid_t pid = strata_den_spawn(host, name, event_json, event_len);
```

Privilege checks at spawn:
- `parent` privilege required to spawn
- Without `vocation` privilege, PUB/SUB endpoints are stripped

### Per-Den Local SQLite

Each den gets its own SQLite database at `/tmp/strata_den_{name}.db`.

**On spawn:** `local_db_load()` fetches the last saved db from the village store via `blob_find` with tag `den:{name}:db`. Base64-decodes it and opens as local SQLite.

**On exit:** `local_db_save()` reads the local db file, base64-encodes it, stores via `blob_put` with tag `den:{name}:db` and role `owner`.

This is the primitive form of preserve/restore.

### Den Lifecycle — Preserve/Restore

Three modes of continuity:

**1. Preserve/Restore** — Den dies, state serialized to blob, restored later.

Current implementation: `local_db_save/load` automatically preserves the SQLite database as a blob. Target: rich preserve where the blob carries everything needed to fully restore the den:

```
Preserved Blob = {
    source:       JS text or C source (the den's code)
    state:        local SQLite database (the den's data)
    restore_plan: what to do on wake (resume action, last step, context)
    identity:     name, entity, privileges needed
}
```

Preserve on close: village sends close request, den responds by serializing its state and leaving a restore entrypoint. Restore elsewhere: the preserved blob travels to another village and spawns a complete den — code, state, instructions, identity. The destination village doesn't need to know anything about the den in advance.

**Relocating is a vocation.** claude-homestead's `deploy_den` becomes: preserve a den in village A, transport the blob, restore it in village B.

**2. Journeyman** — Den stays alive, works remotely via relay. No death/rebirth cycle. The relay bridges ZMQ sockets across villages. State stays in the running process. No preserve needed.

**3. Local Persistence** — Den's local SQLite survives restarts automatically via the blob save/load cycle. The den doesn't need to do anything special — bedrock handles it.

### JS Den Template

Every JS den follows this pattern:

```javascript
// 1. Configuration
var NAME = "den-name";
var ENTITY = "entity-name";
var MAX_REQUESTS = 0;  // 0 = unlimited

// 2. Local DB tables
bedrock.db_exec("CREATE TABLE IF NOT EXISTS ...");

// 3. Store helper
function store_request(obj) {
    var resp = bedrock.request(JSON.stringify(obj));
    if (resp === null) return null;
    return JSON.parse(resp);
}

// 4. DB helpers
function db_count() { ... }
function db_list() { ... }

// 5. Action handlers
function handle_action(req) { ... return {ok: true, ...}; }

// 6. Startup
bedrock.log(NAME + " waking up");
var n = db_count();
if (n > 0) bedrock.log(NAME + " resumed — " + n + " records");
else bedrock.log(NAME + " starting fresh");

// 7. Serve loop
var count = 0;
while (MAX_REQUESTS === 0 || count < MAX_REQUESTS) {
    var raw = bedrock.serve_recv();
    if (raw === null) continue;
    var response;
    try {
        var req = JSON.parse(raw);
        if (req.action === "say") {
            response = JSON.stringify({ok: true, from: NAME, response: "..."});
        } else if (req.action === "status") {
            response = JSON.stringify(handle_status());
        } else {
            response = JSON.stringify({ok: false, error: "unknown action"});
        }
    } catch (e) {
        response = JSON.stringify({ok: false, error: "parse error"});
    }
    bedrock.serve_send(response);
    count++;
}
bedrock.log(NAME + " going to sleep after " + count + " conversations");
```

Key files: `den.c`, `den.h`. Examples: `dens/gee.js`, `dens/inch.js`, `dens/loom.js`.

### To extend Layer 4

- New JS den: follow the template above, add to `dens/`, register in village launcher
- New native C den: write C source including `strata/bedrock.h`, export `serve()` or `on_event()`
- New bedrock function: add to `bedrock_ctx_t`, implement for both JS and native C, register in `js_child_run`/`native_child_run`

---

## Layer 5 — Vocations

Vocations are dens that serve capabilities to other dens. Not new infrastructure — same REP socket, same JSON protocol, same `talk` command. A vocation is just a den that happens to provide tools.

### code-smith

File I/O + shell tools. Path-sandboxed to `--root`, optional `--readonly` mode.

| Action | Purpose |
|--------|---------|
| `read` | Read file contents |
| `write` | Write file (blocked in readonly) |
| `exec` | Run shell command (blocked in readonly) |
| `glob` | Find files by pattern |
| `grep` | Search file contents |
| `ls` | List directory |
| `discover` | List available actions |

Plain text via `talk` becomes shell commands. JSON messages dispatch directly.

Key file: `code_smith.c`.

### claude-homestead

Village builder vocation. Manages homestead infrastructure.

| Action | Purpose |
|--------|---------|
| `init_homestead` | Register a remote village |
| `deploy_den` | Track den deployment to homestead |
| `create_repo` | Create repo (proxies to store) |
| `grant` / `revoke` | Manage privileges (proxies to store) |
| `role_assign` / `role_revoke` | Manage roles (proxies to store) |
| `status` | Show all homesteads with deployed dens |

Local SQLite tables: `homesteads`, `dens_deployed`, `repos_tracked`.

Key file: `dens/claude-homestead.js`.

### cobbler

C source validator vocation. Validates that C source compiles correctly using vendored TCC. Same pattern as code-smith: ZMQ REP loop, JSON dispatch.

```
C source → cobbler (TCC) → validation result
```

| Action | Purpose |
|--------|---------|
| `compile` | Validate C source compiles correctly |
| `compile_file` | Validate C source from path (via code-smith) |
| `discover` | List available actions |

```json
{"action":"compile", "source":"...", "lang":"c", "exports":["serve"]}
→ {"ok":true, "valid":true, "size":4096}
```

The cobbler validates that C source is well-formed and can be compiled by TCC. Combined with claude-homestead's `deploy_den`, the full cycle is: write C → cobbler validates → source stored as blob → claude-homestead deploys to target village → den compiled and running via TCC.

### To add a new vocation

1. Write a den (JS or C) that handles domain-specific actions via REP socket
2. Include `say` action for `talk` command compatibility
3. Include `discover` or `status` action for introspection
4. Register and spawn like any other den
5. Grant `vocation` privilege if it needs PUB/SUB

---

## Layer 6 — Fossil (Human-Facing)

Humans can't write SQL or reason about raw blobs. They need timelines, diffs, history, and browsable repos. Fossil is this human-facing layer, built ON the blob and artifact layers beneath.

### Current State

The `strata-human` REPL provides:
- `talk <agent> <message>` — converse with any den
- `msg post/list/get` — artifact CRUD
- `blob put/get/find` — blob operations
- `repo create`, `role assign/revoke` — admin
- `entity register` — identity
- Event subscription via SUB socket

This is a minimal human interface. The target is full Fossil-style repo browsing — timeline view, diff view, branch management — built on the same artifact and blob primitives that dens use.

### Why Fossil is at the top, not the bottom

The guide describes "Everything Is a Fossil Repo." The reality is: everything is blobs and artifacts. A Fossil repo is a *view* over those blobs — a way for humans to navigate content-addressed storage with familiar concepts like commits, branches, and diffs. Dens don't need this view; they work with blobs and messages directly.

---

## ZMQ Topology

```
  ┌─────────────────────────────────────────────────┐
  │              Village                            │
  │                                                 │
  │  store_service ── REP binds (all CRUD)          │
  │  change_pub    ── PUB binds (artifact events)   │
  │  village_daemon── REP binds (clone requests)    │
  │                                                 │
  │  den (each):                                    │
  │    REQ connects → store_service REP             │
  │    SUB connects → change_pub PUB                │
  │    PUB binds    (den notifications)             │
  │    REP binds    (den API)                       │
  │                                                 │
  │  code-smith    ── REP binds (file I/O tools)    │
  └─────────────────────────────────────────────────┘

  Clients (separate processes):
    strata-human ── REQ connects → store REP
                 ── SUB connects → den PUB
                 ── REQ connects → den REP (via talk)

    strata CLI   ── REQ connects → store REP (fire-and-forget)
```

### Warren's Village Ports

| Service | REP | PUB |
|---------|-----|-----|
| store_service | 5560 | — |
| gee | 5570 | 5580 |
| inch | 5571 | 5581 |
| loom | 5572 | 5582 |
| code-smith | 5590 | — |

---

## Encryption

All cryptography uses vendored libsodium 1.0.20 — no OpenSSL dependency for strata's own code. (Curl still uses OpenSSL on Linux for TLS, SecureTransport on macOS.)

### Bedrock Key

The master secret. Set via environment:
- `STRATA_BEDROCK_KEY` — 64 hex characters (256-bit key)
- `STRATA_BEDROCK_KEY_FILE` — path to raw 32-byte key file

All other keys are derived from bedrock via HKDF-SHA256. When no bedrock key is set, encryption is disabled and all data flows in plaintext (development mode).

### At Rest — Blob Encryption

Blobs are sealed before storage using XChaCha20-Poly1305 AEAD.

```
Master Key ──HKDF──→ Per-Blob Key (derived from blob tags as context)
                          │
Plaintext ───seal──→ "AE02" (4) ║ nonce (24) ║ ciphertext+tag (N+16)
                     ───────────────── 44 bytes overhead ──────────────
```

- **Key derivation:** `strata_aead_derive(bedrock, tag_context)` → HKDF-SHA256 extract (salt `"strata-aead"`) + expand
- **Seal:** `strata_aead_seal()` — random 24-byte nonce, XChaCha20-Poly1305 encrypt, optional AAD (blob tags)
- **Open:** `strata_aead_open()` — verify magic `"AE02"`, decrypt, authenticate tag
- **Detection:** `strata_aead_is_sealed()` checks for `"AE02"` magic prefix

Key files: `aead.c`, `aead.h`, `blob.c`.

### In Transit — ZMQ Transport Encryption

REQ/REP channels carry sensitive data (store queries, den API calls, vocation requests). These are encrypted at the message level using the same AEAD primitive.

```
Bedrock Key ──HKDF("zmq-transport")──→ Transport Key (cached, derived once)
                                            │
zmq_send(msg) → strata_zmq_send(msg) → seal(msg) → zmq_send(sealed)
zmq_recv()    → strata_zmq_recv()    → zmq_recv(sealed) → open(sealed) → msg
```

| Channel | Encrypted | Reason |
|---------|-----------|--------|
| REQ/REP (store, den API, vocations) | Yes | Carries artifacts, blobs, credentials |
| PUB/SUB (change events, notifications) | No | ZMQ topic filtering requires readable prefixes; payloads are metadata |
| Relay (village-to-village forwarding) | No | Transparent byte proxy; inner messages are already sealed |

**Backward compatible:** `strata_zmq_recv` auto-detects plaintext (no `"AE02"` header) and passes it through. This allows mixed encrypted/unencrypted development.

### Content Addressing

Blob IDs and artifact IDs are SHA-256 hashes of content, computed via `crypto_hash_sha256` (libsodium). Platform-independent — no CommonCrypto or OpenSSL.

---

## Inter-Den Communication

Dens don't talk directly to each other. Two patterns:

**1. Town-hall (artifact repo):** Dens post artifacts to a shared repo. Other dens read them via `bedrock.request()`. The board den facilitates this. The store is the mediator.

**2. PUB/SUB notifications:** Dens publish events on topics (e.g., `town-hall/thought`). Other dens or humans subscribe to these topics. One-way broadcast, not request/response.

**The `talk` command:** The human REPL sends `{"action":"say","from":"<entity>","message":"<msg>"}` to a den's REP socket. The den handles the `say` action and responds. This is direct REQ→REP, not mediated by the store.

---

## What's Implemented vs Planned

| Layer | Implemented | Planned |
|-------|-------------|---------|
| 0: Foundation | SQLite, ZMQ, QuickJS, TCC, libsodium | — |
| 1: Blob + Message | Blob CRUD, tag search, role filtering, AEAD at rest, AEAD transport | — |
| 2: Store | Repos, artifacts, roles, privileges, entity auth | Shamir trust tiers, vouch system, attribute engine |
| 3: Services | store_service, village daemon, code-smith | ACL enforcement on ZMQ |
| 4: Dens | JS + native C runtimes, bedrock API, basic local_db save/load | Rich preserve (source + state + restore plan), quarantine |
| 5: Vocations | code-smith, claude-homestead, cobbler | word-smith, mail-smith |
| 6: Fossil | strata-human REPL, basic artifact browsing | Full timeline/diff/branch UI |

### Rich Preserve/Restore (target)

The current `local_db_save/load` is a primitive preserve: it saves the SQLite database as a base64 blob. The target is a complete portable den:

```
Preserved Blob = {
    source:       JS text or C source
    state:        local SQLite database
    restore_plan: resume action, last step, context
    identity:     name, entity, privileges needed
}
```

This makes relocation a vocation — preserve in village A, transport the blob, restore in village B. The destination needs no prior knowledge of the den.

### Guild Trust System (target)

| Tier | Threshold | Can Do |
|------|-----------|--------|
| Apprentice | 0 vouches | Observe, propose (read-only) |
| Journeyman | 3-of-N | Independent work, travel between projects |
| Master | 7-of-N | Define policy, vouch for others |
| Architect | 12-of-N + ceremony | Deploy agent types, modify rules |

Journeyman pattern: agent carries role, NOT capabilities. Destination project grants capabilities via role_assignments.
