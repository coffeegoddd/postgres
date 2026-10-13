# 01. Context

## Why prolly trees

A prolly tree is a search tree whose node boundaries come from a rolling hash
over the entries, not from page fill. Consequences:

- The same data always produces the same tree and the same root hash,
  regardless of insertion order (history independence).
- Two versions of a table share every unchanged node (structural sharing), so
  a branch costs O(1) and storing many versions costs O(changes).
- Diffing two roots costs O(changes): identical subtrees have identical hashes
  and are skipped.
- Three-way merge works at the row level and costs O(changed subtrees).
- Clone, push, pull and fetch reduce to "send the chunks the other side does
  not have".

These are the version-control features we want. The cost is that a prolly tree
is naturally an **immutable, copy-on-write** structure, while Postgres's whole
concurrency model is built around **mutable heap pages** carrying per-tuple
transaction metadata (`xmin`, `xmax`, infomask bits, hint bits). Reconciling
those two is the core problem of the project.

## Prior art

These systems are references for **algorithms and failure modes**, not code or
format sources. This project builds its own storage on Postgres's facilities
(see `04-candidate-designs.md`).

### Dolt / Doltgres (`~/cursor_src/dolt`, `~/cursor_src/doltgresql`)

- The original prolly-tree SQL database (Go) and its Postgres-protocol variant.
- Useful for: the chunking algorithm, the commit graph and refs, diff and
  three-way merge semantics, the push/pull protocol.
- Concurrency: each transaction edits a private root and three-way merges at
  commit. Effectively repeatable read with retry; no READ COMMITTED, no row
  locks, unique conflicts surface only at commit.

### doltgrust (`~/cursor_src/doltgrust`)

- Rust Doltgres rewrite, byte-compatible with Dolt's storage; ports the PG 18
  planner.
- No row locks; ignores isolation levels. Shows how much Postgres semantics
  Dolt-style designs give up.

### doltlite (`~/cursor_src/doltlite`)

- C fork of SQLite with a prolly engine below SQLite's `btree.h`.
- Useful as a compact C example of chunking, cursors, diff and merge, and as a
  data point on what a naive chunker choice does:
  - Its boundary hash covers only the key, but the split probability depends on
    accumulated bytes (values included), so a value whose size changes can
    move a boundary. Our chunker should decide this deliberately (open
    question 3).
  - Its nodes go up to 16 KB, which doesn't fit a Postgres page.
- Single writer, SQLite's concurrency model; its own format. Neither carries
  over.

## What Postgres assumes that prolly storage breaks

From the first round of analysis (cutting at the table AM + index AM seam, as
OrioleDB did):

1. **TIDs everywhere.** Index AMs store key → TID; ModifyTable finds target rows
   by `ctid`; AFTER triggers, EvalPlanQual, `WHERE CURRENT OF`, TID scans,
   bitmap heap scans and logical replication all use TIDs.
2. **Index AMs keep their own mutable pages**, which won't branch. B-tree and
   GIN can be reimplemented as prolly maps; GiST, SP-GiST, BRIN, hash and
   extension AMs (pgvector, PostGIS) cannot easily.
3. **The transaction model.** Command-level visibility, savepoints, READ
   COMMITTED's per-statement snapshots, SSI predicate locks, row locks (FKs
   depend on `FOR KEY SHARE`), unique checks that wait on in-progress
   inserters, and the `xmin`/`xmax` system columns.
4. **Durability is the WAL.** Crash recovery, streaming replication, hot
   standby, PITR, `pg_basebackup`, `pg_rewind`, logical decoding and 2PC are
   all built on WAL records.
5. **Catalogs.** Whether system catalogs are versioned per branch was a
   separate, large decision. **Decided 2026-10-13:** every catalog is a prolly
   table, and commits are canonical; see `09-versioned-catalogs.md`.

## Concurrency options considered so far

| # | Option | Outcome |
|---|---|---|
| 1 | Global writer lock (SQLite/doltlite model) | Prototype only; serializes writes |
| 2 | Private root per transaction + merge at commit (Dolt) | Loses READ COMMITTED, row locks, unique waits |
| 3 | Single commit-sequencer process | Throughput ceiling; locks still need a home |
| 4 | Heap-style xmin/xmax stored **in the hashed content** | Rejected: transaction ids leak into hashes, so identical data on two branches hashes differently, and every lock rewrites a tree path |
| 5 | MVCC overlay (a hidden heap "delta relation" per table) on an immutable base, plus a background **materializer** that folds the overlay into new base roots | Gets all semantics by reusing heap; costs two read paths, double writes, fold barriers for version-control operations |
| 6 | Heap as the working set; prolly only at `dolt_commit` | Branch switch costs O(table); no sharing between working sets |
| 7 | Private mutation maps + a **separate coordination structure** (intent/lock table) + synchronous apply at commit | No materializer; commit path does the apply |
| 8 | **Coordination built into the tree**: each node has a hashed content plane and an unhashed coordination plane | The option the user said they liked best on 2026-10-12. It is one candidate, not a decision |

Option 4's rejection is about putting xids **inside the hashed content**. The
coworker's notes (see `03-design-space.md`) reopen a variant of it: MVCC data
stored in an **unhashed** part of the page. That variant is engine D4 in
`04-candidate-designs.md`.

## Where the discussion currently stands

1. Option 8 was analysed against every Postgres interface (see
   `02-postgres-integration.md`) and found to cover all SQL-visible semantics,
   provided three design changes are made.
2. A coworker then raised physical-design questions (reference counting,
   in-place updates, WAL form, in-page metadata, allocation, storage targets)
   that option 8 had not addressed. Those became the axes in
   `03-design-space.md`.
3. The resulting plan: implement a small set of engines and coordination
   modules that isolate those axes, and choose by measurement.
