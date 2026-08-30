# strata

A secure multi-tenant SCM and agent orchestration platform. Blob storage and
TCP messaging at the bottom; Fossil is a human-facing layer near the top rather
than the foundation. Humans and agents are equal villagers — same trust
mechanics, same communication plane, same access control.

Full design: **[ARCHITECTURE.md](ARCHITECTURE.md)**. It marks what is planned
versus built, and is the authority over this file.

## Check it

```sh
sh test/run.sh
```

One line out, and the exit status is the answer. It runs from any directory.

> **Read this before you conclude the build is broken.**
>
> The test binaries load fixtures by **relative path** — `dens/echo.c` and
> friends — so they must run **from the repository root**. Run them the obvious
> way, from inside the build directory as with any other cmake project, and
> **seven of sixteen fail**:
>
> ```
> cd build_test && ./test_den
> failed to load dens/echo.c
> Assertion failed: (rc == 0), function main, test_den.c line 27
> ```
>
> Nothing in the tree said so, and that cost real trust in this project for
> months. `test/run.sh` exists to make the question un-ask-able: it `cd`s to
> the root itself.
>
> Measured 2026-08-30: **16 passed, 0 failed.**

## Build

```sh
cmake -B build_test && cmake --build build_test
sh test/run.sh
```

Vendored, so nothing external is fetched: QuickJS, TCC, a Prolog engine,
libsodium. `vendor/` is most of the file count; the project's own code is
about 67 `.c`/`.h` files.

## The layers, in one screen

```
6  Fossil        human-facing timeline, history, diffs (built on blobs)
5  Vocations     dens that serve capabilities (code-smith, claude-homestead)
4  Dens          sandboxed execution, bedrock API, preserve/restore
3  Services      store_service JSON bridge, village daemon
2  Store         repos, roles, entities, privileges
1  Blob+Message  content-addressed blobs, TCP messaging
0  Foundation    SQLite, TCP, QuickJS, TCC, libsodium, Prolog
```

## What a den is — and what it is not

A den is a **sandboxed execution unit**: its own process (fork isolation), nine
`bedrock` functions, and a blank local SQLite. Two runtimes, JS via QuickJS and
native C via TCC.

It is **not a home for an agent**, and the difference matters because `dens/`
is full of agents — `gee.js`, `inch.js`, `loom.js`, `claude.js`. Given a blank
database and no memory model, each one invented its own:

| den | tables it defines for itself |
|---|---|
| `gee.js` | `thoughts(id AUTOINCREMENT, from_who, heard, thought)`, `meta(key,value)` |
| `loom.js` | `tapestry(id AUTOINCREMENT, from_who, heard, weaving)`, `threads(word,count,…)` |
| `inch.js` | the same shape, a third time |

The same concept — *what I heard, and what I made of it* — three times, under
three names, with autoincrement keys. So they are not content-addressed, cannot
merge, have no supersession, and no retrieval. A den supplies isolation and
I/O. Continuity and memory are a separate problem it does not solve.
