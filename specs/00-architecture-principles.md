# 00. Architecture principle: interface → implementation

**This is the most important implementation rule in the project.** Every
component is an interface with one or more implementations behind it. Code
depends on interfaces, never on a particular implementation. The goal is that
any design decision in these specs can be changed later by writing a new
implementation, without rewriting the code around it.

The candidate designs (`04-candidate-designs.md`) are not decided, and many
foundation choices (node format, chunker rule, hash, encodings) will be chosen
by measurement. The code must make that cheap.

## The pattern

Use the pattern Postgres already uses for table AMs (`TableAmRoutine`), index
AMs (`IndexAmRoutine`), storage managers (`smgrsw`) and WAL resource managers
(`RmgrData`):

- **The interface** is a header containing a routine struct of function
  pointers, the opaque types it passes around, and its contract written as
  comments: preconditions, postconditions, error behaviour, concurrency and
  memory rules.
- **Each implementation** is a separate set of source files that fills in that
  struct. Its internal headers are private to it.
- **Registration and selection** happen by name, through a registry. Which
  implementation is used comes from configuration (a GUC, tablespace option,
  reloption or the table AM name), not from code.
- **Callers** include only the interface header and call through the routine
  struct.

## Rules

1. No code outside an implementation includes that implementation's headers or
   calls its functions directly.
2. No implementation depends on another implementation of the same or a
   different interface. Implementations depend only on interfaces.
3. Every interface gets a **stub implementation first** that raises
   `ERRCODE_FEATURE_NOT_SUPPORTED`. Stubs are how roadmap step 1 is built.
4. Every interface gets a **conformance test suite** that runs against every
   implementation. A new implementation is done when it passes the suite.
5. Anything persisted (pages, WAL records, catalog rows, canonical chunks)
   records **which implementation and format version wrote it**, so data
   written by one implementation can be recognized and refused, migrated or
   read by another.
6. Interfaces are kept as narrow as the callers need. Widening one is a design
   change: record it in these specs.
7. Interface headers are reviewed with more care than implementations, because
   they are the expensive thing to change.

## The interfaces

Each of these is an interface with the implementations listed. Names are
provisional.

| Interface | Responsibility | Implementations planned |
|---|---|---|
| **Engine** | The tree as a whole: cursors, private writes, commit, checkpoint, redo, reclamation (`02-postgres-integration.md` section C) | D1, D2, D3a, D3b, D4, (D5), stub |
| **Coordination** | Row locks, insert intents, range locks, commit markers, SSI mapping | K1, K2, K3, stub |
| **Node page format** | Layout of a node in a page: canonical region, unhashed special space, item access | One per engine family |
| **Chunker / boundary rule** | Where node boundaries fall; forced splits | Key-only, key + bytes |
| **Content hash** | Hashing the canonical region | SHA-256, others if measured |
| **Key encoder** | memcmp-comparable keys from btree opclasses and collations | Per opclass, plus a comparator fallback |
| **Value encoder** | Row encoding and deforming into slots | Postgres tuple format, portable canonical format |
| **Node store** | Finding and reading or writing nodes: hash → block map, physical location, object store | Hash-addressed, location-addressed |
| **Allocator** | Space management for in-place engines | Fixed-size blocks; variable-size |
| **Reclamation** | Deciding what is garbage and freeing it | Root-reachability GC, birth epoch, lazy refcounts, in-page vacuum |
| **Log** | What the WAL records contain and how they are replayed | Full chunk, delta, logical |
| **Commit pipeline** | Ordering and publishing roots at commit | Single lock, ordered pipeline |
| **Row identity** | TIDs ↔ keys | Content-derived TID map, wide row ids |
| **Refs and commit graph** | Branches, commits, history storage | Catalog tables, others |
| **Canonical export / transfer** | Producing canonical chunks; clone/push/fetch transport | Local copy, network protocol |
| **Benchmark workload** | One S or V workload | S1–S9, V1–V4 |
| **Metric collector** | One metric source | WAL, diskstats, `pg_stat_io`, perf, engine counters |
| **Result sink** | Where results go | Files under `specs/results/`, others |

The Postgres-facing adapters (the table AM and index AM handlers, the WAL
resource manager and the core-patch hooks) are thin: they translate Postgres
calls into calls on these interfaces and contain no engine logic.

## Performance

Calls through function pointers cost a little in hot paths (cursor next, key
compare, hash). The rule still holds there. Where profiling shows dispatch
matters, the fix keeps the interface: resolve the routine once per scan or
statement and cache it, or batch calls (e.g. return several rows per call), as
Postgres does with `TupleTableSlotOps`. Never remove an interface for speed
without recording the decision in these specs.
