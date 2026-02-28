# CLAUDE.md — Fossil Strata

## What This Is

Fossil Strata is a secure, hyper-lightweight, multi-tenant SCM and agent orchestration platform. It manages thousands of projects and dev teams — some of whom are autonomous agents — under a unified cryptographic trust model. Security is not a feature; it is the architecture.

## Philosophy

- **Zero trust by construction.** The architecture IS the security model. No firewalls, no policy docs — math enforces everything.
- **Simplicity is strength.** Six components, one binary, under 5 MB. If it can't be built from the six foundations, it doesn't belong.
- **Humans and agents are equals under cryptographic law.** Same trust mechanics, same encryption, same audit trail, same sandboxes.
- **Everything is a Fossil repo.** Every project, every agent. Code, knowledge, state, vouches, accuracy records — all artifacts in repos.
- **The bedrock connects; the sandbox isolates.** ZMQ enables any-to-any communication. WASM ensures connectivity doesn't compromise safety.

## The Six Foundations

| Component | Role | Notes |
|-----------|------|-------|
| **SQLite** | Storage | Zero-config, one file per repo. Substitutable with PostgreSQL for city-scale. |
| **Fossil Model** | Repo structure | Content-addressed Merkle tree, immutable timeline, wiki/tickets/artifacts. Reinvented with role-based envelope encryption. |
| **WAMR** | Agent execution | ~50–85 KiB. Interpreter/AOT/JIT. Agents compile from Rust, C, Zig, Go, AssemblyScript, JS to .wasm. |
| **ZeroMQ** | Communication | The sole communication plane ("the bedrock"). PUB/SUB, PUSH/PULL, REQ/REP, DEALER/ROUTER. |
| **AEAD AES** | Encryption | Authenticated encryption. Every artifact at rest, every message on the wire. Always. |
| **Shamir SSS** | Trust & governance | M-of-N secret sharing for key management, vouch accumulation, critical action authorization. |

Nothing else is permitted in the core.

## Architecture At A Glance

```
┌───────────────────────────────────────┐
│           Fossil Strata               │
├───────────────────────────────────────┤
│  WAMR sandboxes (agents)              │  ← Bidirectional fencing
├───────────────────────────────────────┤
│  ZMQ bedrock (communication)       │  ← ACL, encrypted, audited
├───────────────────────────────────────┤
│  AEAD + Shamir (crypto + governance)  │  ← Role-based envelope encryption
├───────────────────────────────────────┤
│  SQLite / PostgreSQL (swappable)      │  ← Village to city migration
├───────────────────────────────────────┤
│  Fossil-model repos (storage + audit) │  ← Immutable timeline
└───────────────────────────────────────┘
```

## Security Model

### WASM Sandboxes (Bidirectional Fencing)
- **Top fence (ceiling):** Agent can't reach up. No filesystem, no network, no syscalls. Only explicitly injected WASM imports.
- **Bottom fence (floor):** Agent can't reach down. WASM linear memory seals each agent in its own address space.
- Agents are stateless between invocations. All persistent state goes through explicit APIs into SQLite.
- Every agent runs in WAMR. No "trusted mode." No "native mode." No exceptions.

### Role-Based Envelope Encryption
- Every artifact encrypted with a random DEK (data encryption key).
- DEK encrypted once per authorized role. Only roles in the envelope can decrypt.
- Unauthorized data is mathematically unreadable, not just access-denied.
- Repos sync fully including encrypted artifacts the recipient can't read. Merkle tree integrity preserved.

### Artifact Envelope Structure
```
content_hash     → hash of plaintext (Merkle tree integrity)
encrypted_blob   → AEAD encrypted content
role_keys[]      → per-role encrypted DEK copies
metadata         → unencrypted (timestamps, type, author) for indexing
```

### ZMQ Bedrock
- Single communication plane. Everything goes through ZMQ. No bypasses, no side channels.
- Every message: AEAD encrypted, sender authenticated, ACL authorized, audit logged to Fossil.
- Patterns: PUB/SUB (events), PUSH/PULL (task queues), REQ/REP (queries), DEALER/ROUTER (routing).
- Transport scales: `inproc://` → `ipc://` → `tcp://` without code changes.

### Shamir Governance
- Role keys reconstructed from shares held by members. Revocation = re-key excluding revoked member.
- Critical actions (deploy, role assignment, policy change) require M-of-N reconstruction.
- No single admin, no root key, no god mode.

## Trust Model: The Guild System

### Trust Tiers
| Tier | Threshold | Can Do | Can't Do |
|------|-----------|--------|----------|
| **Apprentice** | 0 vouches | Observe, propose. Read-only. | Act without human cosign. |
| **Journeyman** | 3-of-N | Independent work, travel between projects. | Define policy, vouch for others. |
| **Master** | 7-of-N | Define policy, vouch for others. | System-level changes without ceremony. |
| **Architect** | 12-of-N + ceremony | Deploy agent types, modify rules. | Anything without Shamir ceremony. |

### Vouch Mechanics
- A vouch = a Shamir share, cryptographically signed, stored in Fossil, linked to evidence.
- Vouches decay (configurable, e.g. 90 days). Threshold must be met with recent vouches.
- Vouch revocation → credential collapse → instant propagation via ZMQ → access revoked everywhere.
- Works identically for humans and agents.

### Journeyman Pattern
- Agent carries its role, NOT its capabilities. Capabilities granted by destination project.
- Same agent, same role → different capability sets at different projects.
- State stays local. WASM sandbox is fresh per engagement. No cross-contamination.
- Scale by adding journeymen to the pool, not per-project infrastructure.

## Access Control: Three Layers

| Layer | Answers | Granularity | Example |
|-------|---------|-------------|---------|
| **Roles** | "What kind of thing are you?" | Organizational, stable | `build-agent`, `senior-dev`, `auditor` |
| **Capabilities** | "What actions can you perform?" | Technical, granular | `read_diff()`, `post_comment()`, `queue_build()` |
| **Attributes** | "What's true about this context?" | Dynamic, contextual | `repo.tag = compliance-critical`, `agent.age < 24h` |

Resolution flow: message → check role → map to capabilities → evaluate attributes → allow/deny/escalate.

### Quarantine
New or updated `.wasm` binaries auto-receive `age < 24h` attribute. Read-only regardless of role until quarantine expires.

## Agent Lifecycle

### Every Agent Is a Fossil Repo
Its repo contains: `.wasm` binary (versioned), learned patterns (wiki/artifacts), state (SQLite), vouch history, accuracy record. The repo IS the agent.

### Apprentice-Manager Loop
1. Apprentice observes diffs, reviews, approvals, rejections. Commits patterns to its own repo.
2. Apprentice flags a finding with confidence score. Publishes via ZMQ.
3. Manager evaluates against its own knowledge + apprentice track record + project context.
4. Human confirms or corrects. Feedback flows back via ZMQ.
5. Pattern confidence adjusts. Positive outcomes count toward next vouch.

No special infrastructure. Just Fossil repos talking through ZMQ.

## Substitutable Storage

```toml
# Village (day one)
[storage]
backend = "sqlite"
path = "/var/strata/repos"

# City (scale)
[storage]
backend = "postgres"
connection = "host=db1.internal dbname=strata"
```

Storage interface: `artifact_put/get/list`, `vouch_store/query`, `metadata_index`, `sync_delta`, `begin/commit/rollback`. Both backends implement the same interface. Zero agent code changes on migration.

Edge nodes (SQLite) federate into central stores (PostgreSQL) through ZMQ sync.

## Open Gateway

When ZMQ endpoints face outward, Strata becomes a protocol. External agents connect as journeymen. Security travels with the agent (sandbox + credentials + encryption), not with the network perimeter.

Enables: marketplace of trusted expertise, paid journeyman engagements, external agent platform integration (Strata as the secure execution layer).

## Build Phases

1. **The Bones** — Encrypted Fossil-model repo engine on SQLite. AEAD envelope encryption, Merkle tree, basic sync.
2. **The Sandbox** — Embed WAMR. Capability injection. First agent reading/writing a repo in a sandbox.
3. **The Bedrock** — ZMQ integration. ACL engine. Event-driven agent execution. Audit logging.
4. **The Guild** — Shamir vouches. Trust tiers. Credential collapse propagation.
5. **The Journey** — Journeyman pattern. Cross-project travel. Attribute engine. Quarantine.
6. **The City** — PostgreSQL backend. Storage abstraction. Edge federation. Migration tooling.
7. **The Gateway** — Public endpoints. External agent handshake. Marketplace. Escrow/settlement.

## Inviolable Constraints

- Single binary, under 5 MB, zero mandatory external dependencies.
- No plaintext at rest. No plaintext on the wire. Ever.
- No agent runs outside a WASM sandbox. No exceptions.
- No single point of authority. All sensitive actions require Shamir M-of-N.
- Immutable audit trail. No rebase, no force push, no history rewrite.
- Capability injection, not capability request. Host decides, not agent.
- Single communication plane (ZMQ). Every message ACL-checked independently.
- Storage-agnostic core. No direct SQLite/PostgreSQL calls in business logic.
- Village to city without rewriting agent code, workflows, or policies.

## Key Mantra

> Fossil's bones, encrypted blood.
> Six components. One binary. Thousands of projects.
> Humans and agents, equals under cryptographic law.
