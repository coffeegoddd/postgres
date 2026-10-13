# Prolly-tree storage for Postgres: specs

This directory holds the working specs for this fork. Read it first if you are
picking up the work cold.

## The goal in one paragraph

This repository is a fork of upstream PostgreSQL (`master`, `20devel`). The goal
is to **keep as much of Postgres as possible** (its SQL, planner, executor,
MVCC semantics, row locks, WAL, replication and extension ecosystem) while
swapping the storage underneath to **prolly trees**, so tables gain Dolt-style
version control: branch, commit, diff, merge, clone, push, pull and fetch.
Dolt and Doltgres drop most of Postgres's concurrency semantics to get version
control; this project's premise is that we should not have to.

## Status (read this before anything else)

- **Only what is marked decided is decided.** Decisions are recorded in
  `08-open-questions.md` (the status table at its top) with their date;
  everything else is a design under discussion.
- **Where the work is:** roadmap step 4, building the first engine (D1+K1).
  Steps 4a (hash, node format, chunker, canonical key and value encoders),
  4b (cluster-wide node store, WAL resource managers) and 4c (copy-on-write
  tree operations) are built; the root layout (question 33) comes next, then
  4d. See the status notes in `07-roadmap.md`.
- **The product goal:** true to Postgres and true to Dolt. Postgres-native
  working sets; canonical, history-independent version-control commits;
  schema versioned with data (`09-versioned-catalogs.md`).
- Code lives in
  `src/backend/access/prolly/` and `src/include/access/prolly_*.h`; the
  benchmark harness in `src/tools/prolly_bench/`.
- The plan is to **build several candidate designs, benchmark them against
  each other and against stock Postgres (heap + nbtree), and choose from the
  measurements**, starting with one engine (roadmap steps 4–6).

## Reading order

| File | What it covers |
|---|---|
| [00-architecture-principles.md](00-architecture-principles.md) | **Read first.** Every component is an interface with implementations behind it; the list of interfaces and the rules |
| [01-context.md](01-context.md) | Background: prolly trees, reference implementations, the options considered so far and why |
| [02-postgres-integration.md](02-postgres-integration.md) | The interfaces any design must satisfy to plug into Postgres, core patches, invariants, what can only be emulated |
| [03-design-space.md](03-design-space.md) | The independent design questions (axes) and how they constrain each other |
| [04-candidate-designs.md](04-candidate-designs.md) | The shared foundation, baseline, engines D1–D5, coordination modules K1–K3, the pairings to build, and what each one tests |
| [05-storage-targets.md](05-storage-targets.md) | How the design is chosen per storage type, and the logical-format invariant that makes that safe |
| [06-benchmarking.md](06-benchmarking.md) | The S (storage) and V (version-control) suites: workloads, metrics, environment, procedure, correctness gates, reporting |
| [07-roadmap.md](07-roadmap.md) | The six-step build order (stubs, harnesses, baseline, first engine, measurement, discussion) and exit criteria |
| [08-open-questions.md](08-open-questions.md) | Questions that are still open, and who or what decides them; decisions made so far |
| `src/test/prolly/README` | The requirements suite: what an engine must pass, and how to run it |
| [09-versioned-catalogs.md](09-versioned-catalogs.md) | Versioned schema and canonical form: the decided design (Postgres-native working set, canonical commits, identity by name), proposals, and what Postgres assumes about heap catalogs |

## Related local repositories

| Path | What it is | Why it matters |
|---|---|---|
| `~/cursor_src/doltlite` | C fork of SQLite with a prolly engine below SQLite's `btree.h` | Prior art only: a working example of chunking, diff and three-way merge in C. Not a code or format source (see below) |
| `~/cursor_src/doltgrust` | Rust rewrite of Doltgres | Byte-compatible with Dolt; ports the PG 18 planner. No locks, ignores isolation levels |
| `~/cursor_src/dolt` | Dolt (Go) | Reference for the storage format, the noms chunk journal and the commit graph |
| `~/cursor_src/doltgresql` | Doltgres (Go) | Postgres wire protocol on top of Dolt's engine |

## Built for Postgres, not ported

This storage is designed for Postgres from the ground up. Dolt, Doltgres,
doltgrust and doltlite are **prior art**: read them to understand algorithms
(content-defined chunking, diff, three-way merge, the commit graph), not as
code to port or formats to adopt. Every component should be built from
Postgres's own facilities: pages, the buffer manager, smgr, WAL and resource
managers, the lock manager, predicate locking, AIO and read streams, memory
contexts, ResourceOwners, btree opclasses, `ereport`, and the Postgres coding
style. `04-candidate-designs.md` describes the Postgres-native foundation in
detail.

## Glossary

| Term | Meaning |
|---|---|
| **Prolly tree** | A B-tree-like search tree whose node boundaries are chosen by a rolling hash over the content, so the same data always produces the same tree. Nodes are content-addressed chunks |
| **Chunk** | A serialized tree node, identified by the hash of its bytes |
| **Root** | The hash (or pointer) of a tree's top node. A root identifies an entire table state |
| **Branch head / working set** | The current mutable root set of a branch: Postgres-native (local OIDs, catalog rows as Postgres stores them) |
| **Version-control commit** | A commit made with `prolly_commit` (Dolt's `dolt commit`), as distinct from a SQL `COMMIT`. Its hash covers the canonical form only |
| **Canonical form** | The OID-free, portable, history-independent form of schema and data that commits' root hashes cover and clone/push transfer. A commit's own id also covers its parents and message, as in Dolt |
| **Content plane** | The hashed, persisted part of a node |
| **Coordination plane** | Unhashed state attached to a node: row locks, insert intents, commit markers |
| **Canonical chunk** | The exact byte form of a node that the hash covers and that push/pull exchange |
| **Engine (D1–D5)** | A candidate physical implementation of the tree. See 04 |
| **Coordination module (K1–K3)** | Where row locks and intents live. See 04 |
| **Table AM / Index AM** | Postgres's pluggable table and index access-method APIs (`tableam.h`, `amapi.h`) |
| **TID** | Postgres's 6-byte row identifier (block, offset) |
| **xmin horizon** | The oldest transaction any snapshot may still need; state older than this can be dropped |
| **Heap / B0** | Stock Postgres storage, the baseline for every benchmark |

## Ground rules for agents

- **Interface → implementation, always** (`00-architecture-principles.md`).
  Code depends on interfaces, never on a particular implementation. This is
  the most important rule in the project.
- Verify any file path, function name or line number in these specs against
  the checkout before relying on it. They were correct when written, but
  upstream moves.
- When a design discussion changes a decision, update the spec file it
  belongs in and the status line in `08-open-questions.md`, not just the
  conversation.
- Do not copy code or formats from doltlite or Dolt. Besides the design reasons
  above, both are Apache-2.0, while Postgres is under the PostgreSQL License.
- Keep Postgres-semantics preservation as the yardstick. A proposal that
  drops a Postgres behaviour needs to say so explicitly.
- **Every bug gets a test** (user, 2026-10-13). Each bug found or hit in this
  code base comes with at least one new test that fails without the fix:
  in `src/test/prolly` if it is a requirement-level behaviour, in
  `src/test/modules/test_prolly` for an interface or implementation, or in
  the harness's unit tests (`src/tools/prolly_bench/tests`).
- **Requirements are tests.** Decided requirements for engines are encoded
  in `src/test/prolly`; when a decision adds or changes a requirement, add
  or change its test there in the same change.
