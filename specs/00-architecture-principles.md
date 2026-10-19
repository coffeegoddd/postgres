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

| Interface | Responsibility | Implementations (built in bold) |
|---|---|---|
| **Chunker / boundary rule** | Where node boundaries fall; forced splits | By the bytes a node holds (**bytes**, step 5; replaced key-only **keyhash**) |
| **Content hash** | Hashing a chunk's bytes | SHA-256 (**sha256**), others if measured |
| **Key encoder** | memcmp-comparable keys from btree opclasses and collations; no local OIDs | Per opclass, plus a comparator fallback (**ordered**) |
| **Value encoder** | Row encoding and deforming into slots | Canonical portable format (**canonical**) |
| **Chunk format** | Layout of a tree node as a chunk of its own length | **basic** |
| **Tree** | Persistent prolly-tree operations: build, get, cursors, batched apply, count, walk, diff | Copy-on-write in the chunk store (**cow**) |
| **Table keying** | How a folded table's rows become tree items: key, value layout, content hash | Primary key, else the row's leading bytes and hash with a count (**pk_or_prefix**, #149; replaced **pk_or_content**) |
| **Chunk store** | A database's chunks and root: appends, root moves, WAL, checkpoints | Append-only journal, conjoined into table files (**journal**) |
| **Table file** | The format of a file of chunks with its index | **basic** |
| **Change finder** | Which rows changed since a fold | From the pages block-mapped storage lists as changed (**pages**) |
| **Change log** | A prolly table's row changes, recorded as they are written, for folds to read | Stub; `19-change-log.md` |
| **Row applier** | Writing rows into a branch's tables when a head moves | As logical replication's apply worker writes them (**replication**) |
| **Refs and commit graph** | Branches, commits, tags, remotes, merge state | In the chunk store's root (**copies**) |
| **Transfer** | Moving commits between stores: what to send, checking what is received | The chunks a commit reaches that the other side lacks (**chunks**) |
| **Remote** | Where a remote's chunks and heads live | A directory (**file**); network and object storage later |
| **Diff / merge** | Row diff and three-way merge between commits, over canonical trees | **folded** |
| **Schema canonicalizer** | Deriving the canonical, OID-free schema from catalog rows, folding tables' rows, and moving definitions back | **folded** (`09-versioned-catalogs.md`, `11-pages-and-dolt-snapshots.md`) |
| **Reclamation** | Deciding what is garbage and freeing it | Online GC of the chunk store (step 7) |
| **Benchmark workload** | One S or V workload | S1–S9, V1–V4 |
| **Metric collector** | One metric source | WAL, diskstats, `pg_stat_io`, perf, engine counters |
| **Result sink** | Where results go | Files under `specs/results/`, others |

The working set has no interface of its own here: it is heap's pages, in
block-mapped storage (`11-pages-and-dolt-snapshots.md`, part 5).

**Removed with D1 (#183).** These were the tree engines' interfaces; no
component of the page design used them. Their last code is at
`cff08be5c3`, and `04-candidate-designs.md` describes them.

| Interface | Responsibility | Implementations built |
|---|---|---|
| Engine | The tree as primary storage: cursors, private writes, commit, checkpoint, redo | D1 (D2–D4 stubs) |
| Coordination | Row locks, insert intents, range locks, commit markers | single_writer, K1 (K2, K3 stubs) |
| Node page format | A node in a page, with per-child block hints | basic |
| Node store | Finding and reading or writing nodes: hash → block map | global |
| Root layout | How a branch's root finds every relation's tree | subroots |
| Allocator | Space management for in-place engines | none |
| Log | What the WAL records contain and how they are replayed | chunks |
| Commit pipeline | Branch heads; ordering and publishing roots at commit | serial |
| Row identity | TIDs ↔ row versions | counter |
| Index keying | A prolly index's entries | columns_then_row |

**Diff / merge** was added while building step 1: the version-control SQL
functions needed an interface to call, and diff and merge work on canonical
trees independently of how the working set is stored.

**Tree** was added in step 4c, and is kept: trees are where commits'
canonical form lives (`prolly_tree.h`).

**Table keying** was added in step 4d.1 (question 36): how rows become
tree items is independent of the rest, and other rules stay possible
(`prolly_keying.h`).

The page design's interfaces (chunk store, chunk format, table file, change
finder, row applier, remote) were added with its steps 3–6.

The C interfaces (everything above the benchmark rows) are headers
`src/include/access/prolly_*.h`. The benchmark workload, metric
collector and result sink interfaces belong to the harness and are defined in
step 2.

The Postgres-facing adapters (the table AM handler, the WAL resource
managers BlockMap and ProllyChunks, and the core-patch hooks) are thin: they translate Postgres calls
into calls on these interfaces and contain no storage logic.

## Performance

Calls through function pointers cost a little in hot paths (cursor next, key
compare, hash). The rule still holds there. Where profiling shows dispatch
matters, the fix keeps the interface: resolve the routine once per scan or
statement and cache it, or batch calls (e.g. return several rows per call), as
Postgres does with `TupleTableSlotOps`. Never remove an interface for speed
without recording the decision in these specs.
