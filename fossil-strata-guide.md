

**FOSSIL STRATA**

Philosophy, Requirements & Design Constraints

Build Cycle Orientation Guide

A secure, lightweight, multi-tenant SCM platform

for thousands of projects, human developers, and autonomous agents.

Version 0.1 — Draft

February 2026

# **1\. Philosophy**

## **1.1 Core Thesis**

Fossil Strata is a secure, hyper-lightweight, multi-tenant source control and agent orchestration platform. It manages thousands of projects and development teams — some of whom are autonomous agents — under a unified cryptographic trust model. Security is not a feature; it is the architecture itself.

## **1.2 Founding Principles**

**The Guild Model**

Strata adopts the medieval guild metaphor as its organizing principle. Agents and humans progress through trust tiers — Apprentice, Journeyman, Master, Architect — not by administrative decree but by earning cryptographic vouches from peers who have directly observed their work. Trust is earned through consensus, enforced by mathematics, and recorded immutably.

**Zero Trust by Construction**

Strata does not achieve security through configuration, firewalls, or policy documents. The architecture is the security model. Fork isolation and OS-level sandboxing fence every agent bidirectionally. Role-based encryption makes unauthorized data mathematically unreadable, not merely access-controlled. The ZMQ bedrock is the sole communication plane, ensuring every interaction is encrypted, authenticated, authorized, and audited. There are no dark corners.

**Simplicity Is Strength**

The entire platform compiles to a single binary. Six foundational components — SQLite, Fossil-model repos, TCC, ZMQ, AEAD AES, and Shamir secret sharing — compose into every capability the system needs. No external services, no heavyweight dependencies, no configuration complexity. If a feature cannot be built from these six components, it does not belong in Strata.

**Humans and Agents Are Equals Under Law**

The same trust mechanics, encryption model, audit trail, and access control govern both human developers and autonomous agents. Neither receives special treatment. Both earn trust through the same vouch system, communicate through the same bedrock, and are constrained by the same sandboxes and role-based encryption.

**Everything Is a Fossil Repo**

Every project is a Fossil repo. Every agent is a Fossil repo. An agent’s code, learned knowledge, state, vouch history, and accuracy record are artifacts in its own repo. A project’s code, tickets, wiki, policy rules, and audit trail are artifacts in its repo. There is no separate knowledge layer, no external state store, no metadata database. The repo is the unit of truth.

**The Bedrock Connects; The Sandbox Isolates**

ZMQ is the universal connective tissue — the bedrock through which all entities communicate. It enables any-to-any communication with ACL-governed access. Simultaneously, fork isolation and OS-level sandboxing ensure that connectivity does not compromise safety. An agent can reach the entire network through ZMQ but can only act within the narrow aperture of the bedrock functions injected into its process. Connection without compromise.

# **2\. Architecture Overview**

## **2.1 The Six Foundations**

Strata is built from exactly six foundational components. Every feature, workflow, and security mechanism is composed from these primitives. Nothing else is permitted in the core.

| Component | Role | Why This Choice |
| :---- | :---- | :---- |
| **SQLite** | Storage engine | Zero-config, single-file databases. Each repo is one file. Trivially portable, backupable, replicable. No connection pooling, no ORM, no database server. |
| **Fossil Model** | Repository structure | Content-addressed Merkle tree, immutable timeline, integrated wiki/tickets/artifacts. Reinvented with role-based envelope encryption at the storage layer. |
| **TCC** | Agent execution | Vendored \~100 KB C compiler. Compiles C source to native code at startup. Each agent executes in a forked process with OS-level syscall filtering (seccomp-bpf on Linux, Seatbelt sandbox\_init on macOS). Bedrock functions are injected via tcc\_add\_symbol(). |
| **ZeroMQ** | Communication | Brokerless, lightweight messaging. PUB/SUB, PUSH/PULL, REQ/REP, DEALER/ROUTER patterns. The sole communication plane for all entities. TCP for distributed, inproc for local. |
| **AEAD AES** | Encryption | Authenticated encryption with associated data. Confidentiality and integrity in one primitive. Every artifact at rest, every message on the wire — always encrypted, always tamper-evident. |
| **Shamir SSS** | Trust & governance | M-of-N secret sharing for key management, role assignment, vouch accumulation, and critical action authorization. No single point of authority or compromise. |

## **2.2 Substitutable Storage Layer**

Strata abstracts its storage behind a thin interface that supports both SQLite and PostgreSQL. This enables migration from village-scale (single machine, file-based repos) to city-scale (clustered, high-concurrency, cross-repo queries) without rewriting agent code, policies, or workflows.

| Scale | SQLite (Village/Town) | PostgreSQL (City) |
| :---- | :---- | :---- |
| **Concurrency** | Single writer, multiple readers | Full MVCC, many concurrent writers |
| **Repo model** | One file per repo | Schema-per-repo or database-per-repo |
| **Cross-repo query** | Requires opening multiple files | Single SQL query with joins |
| **Replication** | File copy, rsync | Streaming or logical replication |
| **Deployment** | Single binary, zero config | Requires running service |
| **Best for** | Small teams, edge nodes, laptops, CI runners | Thousands of projects, clustered infrastructure |

The storage interface exposes only fundamental operations: artifact put/get/list, vouch store/query, metadata indexing, sync deltas, and transaction control. Both backends implement the same interface. Agents and workflows are storage-agnostic. Edge nodes running SQLite can federate into a central PostgreSQL city through ZMQ sync.

# **3\. Security Model**

## **3.1 Bidirectional Fencing (Fork Isolation + OS Sandbox)**

Every agent executes in a forked process with OS-level syscall filtering. seccomp-bpf (Linux) or Seatbelt sandbox\_init (macOS) restricts the process to whitelisted syscalls. This provides bidirectional fencing:

* **Top fence (ceiling):** The OS sandbox prevents the agent from reaching up past its process. No filesystem access, no exec, no fork, no network — only the whitelisted syscalls required for the bedrock API. Only the capabilities explicitly injected via tcc\_add\_symbol() are available. A code review agent gets read\_diff() and post\_comment() and nothing else.

* **Bottom fence (floor):** Process isolation seals each agent in its own address space. The agent cannot reach down into the host runtime, the crypto layer, other repos, or other agents’ memory. Each forked process has a separate address space. No cross-process memory access, no shared state.

Agents are stateless forked processes with local SQLite for state. All persistent state passes through explicit APIs into the per-den local SQLite database. Agent crashes or restarts lose nothing. State can be snapshotted, migrated, or rolled back because it is just rows in SQLite, versioned by Fossil.

## **3.2 Role-Based Envelope Encryption**

Fossil Strata reinvents the Fossil storage layer with role-based encryption. Every artifact is encrypted with a random data encryption key (DEK). The DEK is then encrypted once per authorized role using that role’s key. This is standard envelope encryption, with the envelope list determined by policy at commit time.

An agent with the developer role possesses the developer role key and can decrypt the DEK for any artifact that includes developer in its role\_keys list. Artifacts encrypted only to security-auditor are mathematically unreadable to it — not “access denied” but cryptographically impossible.

Repos can be fully synced including encrypted artifacts the recipient cannot read. The structure and integrity of the Merkle tree travel intact; only the readable slice is decryptable. This enables journeyman agents to carry full repo structure while seeing only their authorized layer.

**Artifact Envelope Structure**

* **content\_hash:** Hash of the plaintext, preserving Merkle tree integrity for anyone holding the key.

* **encrypted\_blob:** The artifact content, AEAD encrypted with the DEK.

* **role\_keys:** Per-role encrypted copies of the DEK. Only roles listed can decrypt.

* **metadata:** Unencrypted timeline data (timestamps, artifact type, author). Sufficient for indexing and routing without decrypting content.

## **3.3 The ZMQ Bedrock**

ZeroMQ serves as the sole communication plane. There is exactly one way for any entity to communicate: through the bedrock. This creates a single enforcement point for ACLs, encryption, and audit.

Every message transiting the bedrock is: (1) encrypted with AEAD AES, always, no plaintext ever; (2) authenticated with verified sender identity; (3) authorized by ACL check before delivery; (4) logged with a hash into the Fossil timeline for full reconstructability.

ZMQ socket patterns map directly to Strata’s communication needs:

* **PUB/SUB:** Event broadcast. Commit events, ticket changes, build notifications. ACL determines subscription permissions.

* **PUSH/PULL:** Work distribution. Task queues for agent pools with natural load balancing.

* **REQ/REP:** Synchronous queries. Agent requests a diff, receives the response.

* **DEALER/ROUTER:** Complex routing. Review requests routed by project, language, or expertise.

## **3.4 Shamir Trust Governance**

Shamir’s Secret Sharing eliminates single points of authority throughout the system:

* **Role key management:** Role keys are reconstructed from shares held by role members. Revoking access means re-keying and redistributing shares excluding the revoked member.

* **Critical action authorization:** Deploying to production, assigning sensitive roles, or modifying attribute rules requires M-of-N share reconstruction. No single administrator can act unilaterally.

* **Vouch accumulation:** Each vouch for an agent is a Shamir share. Trust credentials only reconstruct when sufficient independent vouches are gathered (see Section 4).

* **ZMQ topic encryption:** Some bedrock topics require reconstructed keys to access. The deploy/production topic might require 3-of-5 team leads to reconstruct the decryption key.

# **4\. Trust Model: The Guild System**

## **4.1 Trust Tiers**

Trust is not granted by an administrator. It is earned through consensus and enforced by cryptography. Agents and humans progress through four tiers:

| Tier | Threshold | Capabilities | Constraints |
| :---- | :---- | :---- | :---- |
| **Apprentice** | 0 vouches | Read-only observation. Can propose actions but cannot execute them. | All output requires human cosign. Quarantine period on deployment. Learning-only mode. |
| **Journeyman** | 3-of-N | Independent work within sandbox. Capabilities granted by destination project policy. | Cannot define policy. Cannot vouch for others. Subject to project-local attribute rules. |
| **Master** | 7-of-N | Can define policy. Can vouch for other agents. Expanded capability grants. | Vouch power is logged and auditable. Subject to elevated scrutiny. |
| **Architect** | 12-of-N \+ ceremony | Can deploy new agent types. Can modify attribute rules. System-level changes. | Requires Shamir ceremony for every action. Extremely few entities hold this tier. |

## **4.2 Vouch Mechanics**

A vouch is a Shamir share, cryptographically signed and stored as an immutable artifact in Fossil. Each vouch is linked to specific evidence — the actual commits, reviews, or findings that earned it.

* **Accumulation:** An agent completes work, a project lead reviews and vouches. The vouch is a share. Over time, shares from different humans across different projects accumulate. When the threshold is met, the credential reconstructs and a new trust tier unlocks.

* **Decay:** Vouches have expiry. A vouch from six months ago carries less weight than one from last week. The threshold must be met with recent vouches — e.g., at least 3 of 5 from the last 90 days.

* **Revocation:** A vouch can be revoked. If this drops the agent below threshold, the credential collapses immediately. Access revoked across the entire bedrock because the reconstructed key no longer reconstructs.

* **Self-healing:** Vouch revocation propagates through ZMQ instantly. Every project the agent currently accesses sees the credential collapse in real time. All current work by the agent is quarantined for review. No administrator intervention required.

* **Provenance:** Every vouch points to specific work in Fossil. The chain of trust is fully traceable: why was this agent trusted? Trace the vouches to the work.

## **4.3 The Journeyman Pattern**

A journeyman is a skilled agent that travels between projects, bringing expertise where needed. It is not bound to a single repo. It connects to the bedrock, presents its credentials, and works wherever its vouches and role permit.

The journeyman carries its role but not its capabilities. Capabilities are granted by the destination project based on its local policy. The same agent with the same role receives different capability sets depending on where it works. Same agent, same trust, different permissions.

State stays local. The journeyman does not carry context from one project into another. Its forked process is fresh for each engagement, with an empty local SQLite. Persistent findings flow through ZMQ into the project’s own Fossil timeline. No cross-contamination, no information leakage.

Scaling is achieved by adding journeyman agents to the pool, not by creating per-project infrastructure. A new project spins up, defines its policies, and immediately accesses the existing pool of journeymen through the bedrock.

# **5\. Access Control: Three-Layer Model**

Access control operates at three complementary layers. These are not competing models — they operate at different granularities and solve different problems.

## **5.1 Roles (Organizational, Stable)**

Roles answer: “What kind of thing are you?” A build agent. A senior developer. A security auditor. Roles are the language team leads think in. They are easy to reason about, assign, and audit. Role assignment for sensitive roles requires Shamir reconstruction.

## **5.2 Capabilities (Technical, Granular)**

Capabilities answer: “What specific actions can you perform right now?” Read this repo. Write to that branch. Subscribe to these ZMQ topics. Capabilities are what the bedrock functions injected via tcc\_add\_symbol() actually enforce. The sandbox does not know about roles — it knows that this agent has read\_diff() and nothing else. Roles map to capability sets.

## **5.3 Attributes (Contextual, Dynamic)**

Attributes answer: “What is true about this context right now?” The request is outside business hours. This repo is tagged compliance-critical. The agent was deployed less than 24 hours ago. Attributes modify the effective capability set dynamically.

The resolution flow for any action: (1) the agent sends a message on ZMQ; (2) the ACL engine checks the agent’s role; (3) the role maps to capabilities — is this action in the set? (4) attributes are evaluated — does the context allow, restrict, or escalate? (5) the action is allowed, denied, or routed to additional authorization via Shamir.

**Quarantine Example**

When a new agent C source is deployed, it receives an automatic attribute: age \< 24h. The attribute layer restricts it to read-only capabilities regardless of its role. It can observe, learn, and build context but cannot act consequentially until it ages out of quarantine. This safety net costs nothing architecturally because the attribute layer already exists.

# **6\. Agent Lifecycle & Learning**

## **6.1 Every Agent Is a Fossil Repo**

An agent’s Fossil repo is the agent. It contains:

* **Its C source:** Versioned, with full commit history. Every iteration of the agent’s code is preserved and diffable.

* **Its learned patterns:** Wiki entries, structured artifacts, observations from past work. The agent writes what it learns as commits to its own repo.

* **Its state:** SQLite tables inside the repo for operational data.

* **Its vouch history:** Received Shamir shares recorded as artifacts. The agent’s earned reputation.

* **Its accuracy record:** Every flag, every outcome, every correction. Queryable with standard SQL.

## **6.2 The Apprentice-Manager Feedback Loop**

Apprentices learn by observing. They watch diffs, review comments, approvals, and rejections. They commit observed patterns to their own repo. Confidence scores are computed by querying their own history. Over time, patterns with high confidence can be flagged proactively, subject to manager approval.

Managers evaluate apprentice proposals against accumulated knowledge in their own repo, the apprentice’s track record (inspectable via its Fossil repo), and the project’s context and policy. When a human confirms or corrects a finding, the feedback flows back through ZMQ. The apprentice’s pattern confidence adjusts. The manager’s judgment calibrates. Positive outcomes count toward the next vouch.

No special infrastructure is required. The learning loop is just agents committing to their own Fossil repos and reading each other’s repos through the bedrock, governed by the same ACL and encryption as everything else.

## **6.3 Knowledge Portability**

A journeyman’s repo travels with it. When it arrives at a new project, the host can inspect its Fossil timeline to verify its learning history and accuracy. Some patterns are universal and travel freely. Others are project-proprietary and remain local. Knowledge sharing between projects is governed by policy stored in each project’s repo:

* **Private:** Pattern stays in the originating project. The journeyman cannot reference it elsewhere.

* **Anonymized:** Pattern structure travels but identifying details are stripped.

* **Open:** Pattern is freely shared across all projects the journeyman visits.

# **7\. Open Gateway: Strata as Protocol**

## **7.1 From Platform to Protocol**

Strata becomes a protocol when ZMQ endpoints face outward. A journeyman does not need to be inside your infrastructure. It needs only the bedrock. ZMQ runs over TCP. AEAD encrypts everything on the wire. Fork isolation and OS-level sandboxing fence the agent regardless of where it physically runs. Shamir vouches verify anywhere.

The agent’s physical location is irrelevant. Security is in the math and the sandbox, not the network perimeter. There is no perimeter. There is no “inside.” There is only the bedrock, the credentials, and the sandbox.

## **7.2 Integration with External Agent Ecosystems**

Strata can serve as the secure execution layer for external agent platforms. Conversational AI front ends handle natural language, user intent, and channel routing. Strata handles execution, safety, audit, trust, and encryption.

An external agent connects to the ZMQ bedrock as a journeyman. It still communicates through its native interface (chat, API, etc.) but when it needs to touch code, repos, or infrastructure, it goes through Strata. The sandbox constrains it. The ACL governs it. The vouches credential it. The external platform’s weaknesses — broad permissions, prompt injection vulnerability, inability to stop rogue agents — are precisely Strata’s strengths.

## **7.3 The Marketplace**

The open gateway enables a marketplace of trusted expertise. Builders create specialized agents — a Rust security auditor, a React performance optimizer, a HIPAA compliance checker. Agents earn vouches across engagements. Reputation compounds. Better reputation means more work, higher rates, and expanded trust.

Users hire journeyman agents by describing their needs. The bedrock matches them with trusted agents. Work happens in a sandboxed forked process. Results flow back. Payment settles. A vouch is issued. The agent’s reputation grows. The builder earns revenue. The user receives trusted, safe, auditable work.

Disputes are trivially resolvable because every interaction is recorded in the immutable Fossil timeline. What the agent saw, what it produced, what capabilities it had — all cryptographically recorded.

**Marketplace Revenue Model**

* **Bedrock fee:** Small percentage of transactions flowing through ZMQ.

* **Hosting fee:** Builders pay to host agents on Strata infrastructure.

* **Verification fee:** Strata-operated master agents perform deep audits of journeyman agents. “Strata Verified” badge increases trust and rates.

* **Enterprise tier:** Private bedrock segments, dedicated infrastructure, PostgreSQL city-scale, custom policy engines.

# **8\. Design Constraints**

The following constraints are inviolable. Any proposed feature or change that violates these constraints is rejected.

## **8.1 Binary Constraints**

1. Single binary. The entire platform — Fossil-model repos, TCC compiler, ZMQ messaging, crypto layer — ships as one statically-linked executable. Target size: under 5 MB.

2. Zero mandatory external dependencies at runtime. No database server, no message broker, no identity provider, no container runtime. SQLite village mode must work with nothing but the binary and a filesystem.

3. Cross-platform. The binary must compile for Linux (x86\_64, ARM64), macOS, and Windows at minimum.

## **8.2 Security Constraints**

1. No plaintext at rest. Every artifact in every repo is AEAD encrypted. Metadata (timestamps, types, hashes) may be unencrypted for indexing. Content is always encrypted.

2. No plaintext on the wire. Every ZMQ message is AEAD encrypted. No exceptions.

3. No agent runs outside fork isolation and OS sandbox. There is no “trusted mode” or “unconfined mode” for agents. Every agent, including master and architect-tier agents, executes in a forked process with OS-level syscall filtering.

4. No single point of authority. Every sensitive action — role assignment, production deployment, agent promotion, policy changes — requires Shamir M-of-N reconstruction. No administrator account, no root key, no god mode.

5. Immutable audit trail. The Fossil timeline is append-only. There is no rebase, no force push, no history rewrite. Every action by every entity is permanently recorded.

6. Capability injection, not capability request. Agents do not request permissions. The host determines what bedrock functions to inject via tcc\_add\_symbol() based on role, capability set, and attribute context. The agent receives what it is given and nothing more.

## **8.3 Communication Constraints**

1. Single communication plane. All entity-to-entity communication passes through ZMQ. No direct agent-to-agent channels, no bypasses, no side channels. If it does not go through the bedrock, it does not happen.

2. Every message is ACL-checked independently. Chained actions do not inherit authorization. Each hop through ZMQ is independently verified.

3. Every message is logged. The bedrock is fully observable. Every message that transits ZMQ receives a hash recorded in the Fossil timeline.

## **8.4 Storage Constraints**

1. Storage-agnostic core. All repo operations go through the abstract storage interface. No direct SQLite calls or PostgreSQL calls in business logic.

2. One repo \= one unit. Whether SQLite file or PostgreSQL schema, a repo is a self-contained, syncable, backupable unit.

3. Schema simplicity. The repo schema uses standard SQL that runs identically on both SQLite and PostgreSQL. No engine-specific extensions in the core schema.

## **8.5 Agent Constraints**

1. C source compiled by vendored TCC. Agents are written in C and compiled at startup by the vendored TCC compiler to native code. The platform uses a single, well-understood source language with zero external compiler dependencies.

2. Agent identity is its repo. The C source, its hash, learning history, vouch record, and accuracy metrics all live in the agent’s Fossil repo. There is no external identity system.

3. Agent processes are stateless. No state persists in the forked process between invocations. All state goes through explicit APIs into the per-den local SQLite.

4. Agent quarantine on deployment. New or updated agent binaries receive an automatic attribute restriction for a configurable period (default: 24 hours). Read-only capabilities during quarantine regardless of role.

## **8.6 Scale Constraints**

1. Village to city without rewrite. Migrating from SQLite to PostgreSQL requires only a configuration change. No agent code, workflow definition, or policy rule is modified.

2. Journeyman pool scaling. Scaling is achieved by adding agents to the pool, not by creating per-project infrastructure. A new project immediately accesses the existing agent pool.

3. Federation. Edge nodes (SQLite) federate into central stores (PostgreSQL) through ZMQ sync. Encrypted artifacts ensure security regardless of physical data location.

# **9\. Functional Requirements**

## **9.1 Repository Engine (Fossil Strata Core)**

4. Create, clone, sync repos using the Fossil content-addressed artifact model with role-based envelope encryption.

5. Store artifacts as AEAD-encrypted blobs with per-role DEK envelopes.

6. Maintain unencrypted metadata index for timeline construction, search, and routing.

7. Support Merkle tree validation for any role holding the appropriate key.

8. Implement re-keying protocol for role membership changes.

9. Provide policy engine at commit time to determine which roles receive DEK envelopes.

10. Implement encrypted sync protocol supporting full and partial repo synchronization.

## **9.2 Agent Runtime**

7. Vendor TCC and compile C source to native code at agent startup.

8. Implement capability injection: map role \+ capability set \+ attribute context to bedrock functions injected via tcc\_add\_symbol() per invocation.

9. Implement agent lifecycle: deploy, quarantine, activate, suspend, revoke.

10. Support concurrent execution of hundreds to thousands of sandboxed agents.

11. Provide standard host API surface: repo read/write, ZMQ publish/subscribe, state get/set, log.

## **9.3 Bedrock (ZMQ Layer)**

4. Implement ACL engine with role-based, capability-based, and attribute-based rule evaluation.

5. Enforce AEAD encryption on all messages.

6. Implement audit logging with Fossil timeline integration.

7. Support PUB/SUB, PUSH/PULL, REQ/REP, and DEALER/ROUTER patterns.

8. Support transport upgrade: inproc (single process) → ipc (multi-process) → tcp (distributed).

9. Implement public-facing endpoints for open gateway / marketplace operation.

## **9.4 Trust Engine**

4. Implement Shamir secret sharing for vouch generation, accumulation, and credential reconstruction.

5. Implement vouch storage as Fossil artifacts with evidence linking.

6. Implement vouch decay with configurable time windows.

7. Implement vouch revocation with real-time credential collapse propagation via ZMQ.

8. Implement trust tier promotion and demotion based on vouch threshold state.

9. Implement Shamir ceremony protocol for architect-tier actions.

## **9.5 Storage Abstraction**

5. Implement storage interface with artifact CRUD, vouch CRUD, metadata indexing, sync deltas, and transactions.

6. Implement SQLite backend (village/town mode).

7. Implement PostgreSQL backend (city mode).

8. Implement migration tooling: export from SQLite, import to PostgreSQL, with integrity verification.

## **9.6 Marketplace (Open Gateway)**

4. Implement agent discovery through bedrock-published work topics.

5. Implement credential handshake: agent presents Fossil repo for inspection, host verifies vouches, role, and C source hash.

6. Implement escrow and settlement for paid agent engagements.

7. Implement dispute resolution data access: full Fossil timeline evidence for any engagement.

# **10\. Recommended Build Phases**

## **Phase 1: The Bones**

Build the encrypted Fossil-model repository engine on SQLite. Content-addressed artifacts, AEAD envelope encryption, role-based DEK distribution, Merkle tree integrity, basic sync. This is the foundation everything else rests on. Validate by creating, encrypting, syncing, and decrypting repos with multiple role keys.

## **Phase 2: The Sandbox**

Vendor TCC. Implement capability injection from a static configuration. Compile a simple agent from C source to native code, run it in a forked process with OS-level sandboxing (seccomp-bpf on Linux, Seatbelt on macOS), and verify it reads from a repo and writes findings back. Validate that fork isolation + OS sandbox provides bidirectional fencing: the agent can only access injected bedrock functions, cannot escape its process, and produces deterministic output.

## **Phase 3: The Bedrock**

Integrate ZMQ. Implement the ACL engine with role-based rules. Wire agent execution to ZMQ events: a commit triggers an agent via PUB/SUB, the agent reads via REQ/REP, findings publish back via PUB/SUB. All messages AEAD encrypted. Audit log written to Fossil. Validate end-to-end: commit → event → agent → finding → timeline.

## **Phase 4: The Guild**

Implement Shamir vouch system. Vouch generation, accumulation, decay, revocation. Trust tier promotion. Credential collapse propagation. Validate: an apprentice earns vouches, promotes to journeyman, works across two projects with different capability grants, has a vouch revoked, credential collapses, access suspended everywhere.

## **Phase 5: The Journey**

Implement the journeyman pattern. Agents that travel between projects carrying role credentials. Dynamic capability injection based on destination project policy. Attribute engine for contextual rules. Quarantine on deployment. Validate: a journeyman agent audits three different projects, receives different capabilities at each, accumulates vouches, quarantine lifts after 24h.

## **Phase 6: The City**

Implement the PostgreSQL storage backend and the storage abstraction layer. Migration tooling from SQLite to PostgreSQL. Edge federation: SQLite nodes syncing to PostgreSQL central through ZMQ. Validate: seamless migration of a village to city with zero agent code changes.

## **Phase 7: The Gateway**

Expose public ZMQ endpoints. Implement credential handshake for external agents. Marketplace discovery, escrow, settlement, dispute resolution. Integration with external agent platforms as the secure execution layer. Validate: an external agent connects, presents credentials, performs work in a sandbox, results recorded, payment settled.

*"Fossil’s bones, encrypted blood."*

Six components. One binary. Thousands of projects.

Humans and agents, equals under cryptographic law.