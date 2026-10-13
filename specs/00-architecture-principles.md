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
   calls its functions directly. The one exception is the list of built-in
   implementations (`src/include/access/prolly_impllist.h`, like
   `rmgrlist.h`), which the registry reads.
2. No implementation depends on another implementation of the same or a
   different interface. Implementations depend only on interfaces.
3. Every interface gets a **stub implementation first** that raises
   `ERRCODE_FEATURE_NOT_SUPPORTED`. Stubs are how roadmap step 1 is built.
   Concretely, a NULL callback means "not implemented": callers dispatch
   through `PROLLY_CALL(routine, callback, ...)` (`prolly_impl.h`), which
   raises the error naming the interface, implementation and callback. A stub
   is a routine with every callback NULL, and a real implementation can be
   filled in one callback at a time.
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

| Interface | Responsibility | Implementations planned (built so far in bold) |
|---|---|---|
| **Engine** | The tree as a whole: cursors, private writes, commit, checkpoint, redo, reclamation (`02-postgres-integration.md` section C) | D1, D2, D3a, D3b, D4, (D5), stub |
| **Coordination** | Row locks, insert intents, range locks, commit markers, SSI mapping | K1, K2, K3, stub |
| **Node page format** | Layout of a node in a page: canonical region, unhashed special space, item access, per-child block hints | One per engine family (**basic**) |
| **Chunker / boundary rule** | Where node boundaries fall; forced splits | Key-only (**keyhash**), key + bytes |
| **Content hash** | Hashing the canonical region | SHA-256 (**sha256**), others if measured |
| **Key encoder** | memcmp-comparable keys from btree opclasses and collations; no local OIDs | Per opclass, plus a comparator fallback (**ordered**) |
| **Value encoder** | Row encoding and deforming into slots | Canonical portable format (**canonical**, the default since 2026-10-13), Postgres tuple format (**heaptuple**, kept for comparison) |
| **Node store** | Finding and reading or writing nodes: hash → block map, physical location, object store | Cluster-wide hash-addressed (**global**), location-addressed |
| **Root layout** | How a branch's root finds every relation's tree: catalog and user-data sub-roots | Sub-roots keyed by relation OID for the working set (**subroots**); by name for canonical roots (4g) |
| **Tree** | Persistent prolly-tree operations over the foundation: build, get, cursors, batched apply, count, walk, diff | Copy-on-write in a hash-addressed store (**cow**); location-addressed engines (D3, D4) to be decided when they are built |
| **Allocator** | Space management for in-place engines | Fixed-size blocks; variable-size |
| **Reclamation** | Deciding what is garbage and freeing it | Root-reachability GC, birth epoch, lazy refcounts, in-page vacuum |
| **Log** | What the WAL records contain and how they are replayed | Full chunk (**chunks**), delta, logical |
| **Commit pipeline** | Ordering and publishing roots at commit | Single lock, ordered pipeline |
| **Row identity** | TIDs ↔ keys | Content-derived TID map, wide row ids |
| **Refs and commit graph** | Branches, commits, history storage | A refs file per database (proposed; catalogs can't hold refs now that they are versioned), others |
| **Canonical export / transfer** | Producing canonical chunks; clone/push/fetch transport | Local copy, network protocol |
| **Diff / merge** | Row diff and three-way merge between refs, over canonical trees | Structural diff (added in step 1; see below) |
| **Schema canonicalizer** | Deriving the canonical, OID-free schema from catalog rows, and rebuilding local catalogs from it | Catalog-driven, using `BKI_LOOKUP` metadata and the deparser (decided 2026-10-13, `09-versioned-catalogs.md`; to be defined in step 4g) |
| **Benchmark workload** | One S or V workload | S1–S9, V1–V4 |
| **Metric collector** | One metric source | WAL, diskstats, `pg_stat_io`, perf, engine counters |
| **Result sink** | Where results go | Files under `specs/results/`, others |

**Diff / merge** was added while building step 1: the `prolly_diff` and
`prolly_merge` SQL functions needed an interface to call, and diff and merge
work on canonical trees independently of the engine. Canonical export lives on
the Engine interface (`canonical_root`, `export_chunks`); the transfer
interface covers clone, fetch and push.

**Tree** was added in step 4c. Engines build tables, indexes and root sets
from trees, and two engines that share a node store (D1, D2) share tree
code, so the tree operations are their own interface rather than part of
each engine. A tree is an immutable map identified by its root; the
foundation components it uses come from the engine (`prolly_tree.h`).

**Root layout** was added after step 4c (question 33): the two-sub-root
shape of a branch's root is the same for every engine that keeps
hash-addressed trees, and the canonical root will reuse it, so it is an
interface over Tree rather than part of an engine (`prolly_rootlayout.h`).

The C interfaces (everything above the benchmark rows) were defined in step 1:
headers `src/include/access/prolly_*.h`, except the schema canonicalizer,
added to the design on 2026-10-13 and to be defined when step 4g needs it. The benchmark workload, metric
collector and result sink interfaces belong to the harness and are defined in
step 2.

The Postgres-facing adapters (the table AM and index AM handlers, the WAL
resource managers and the core-patch hooks) are thin: they translate Postgres
calls into calls on these interfaces and contain no engine logic.

## Performance

Calls through function pointers cost a little in hot paths (cursor next, key
compare, hash). The rule still holds there. Where profiling shows dispatch
matters, the fix keeps the interface: resolve the routine once per scan or
statement and cache it, or batch calls (e.g. return several rows per call), as
Postgres does with `TupleTableSlotOps`. Never remove an interface for speed
without recording the decision in these specs.
