# 04. Candidate designs

**Status: history.** These were the candidates for prolly trees as primary
storage. The direction changed on 2026-10-15 to Postgres pages with a chunk
store for history (`11-pages-and-dolt-snapshots.md`); D1+K1, the one pairing
built, was removed on 2026-10-17 (#177, #178, #183) and is at the tag
`prolly-d1`. The shared foundation's hash, key and value encoders, trees and
canonical form carried over (`00-architecture-principles.md`).

The axes in `03-design-space.md` multiply into dozens of combinations. Most
axes can be settings inside one engine. Only a few choices change the code
structurally. So the candidates are organized as:

- a **shared foundation**, built once;
- a **baseline** (stock Postgres);
- **engines** (D1–D5): separate implementations, only where structure really
  differs;
- **coordination modules** (K1–K3): pluggable, so one engine can run with
  several;
- **settings**: switches within an engine.

The aim is that every benchmark difference can be traced to one design choice.

## Shared foundation: Postgres-native (built once)

The foundation is designed for Postgres, not ported from Dolt or doltlite. The
starting assumptions below are deliberate defaults; any of them can be
challenged, but a challenge should say what Postgres facility it gives up.

### Where the code lives

- In core: `src/backend/access/prolly/` and `src/include/access/prolly*.h`,
  like `heap/` and `nbtree/`. Not an extension: it needs core patches (B1–B9)
  anyway, and should be a peer of heap.
- Postgres conventions throughout: memory contexts, ResourceOwners for pins,
  `ereport`/`elog`, LWLocks and condition variables, pgindent, `README` files
  per subsystem like `src/backend/access/nbtree/README`.

### Nodes are Postgres pages

- A node is a `BLCKSZ` page with a standard `PageHeaderData`, so data
  checksums, the buffer manager (pinning, eviction, the bgwriter, the
  checkpointer), smgr, AIO and read streams, `full_page_writes` and
  `pg_stat_io` all apply unchanged.
- Nodes are stored in **one cluster-wide node store** (decided 2026-10-13,
  open question 1): fixed relation file numbers in the global tablespace,
  read through smgr and the buffer manager, holding every relation's nodes on
  every branch. A relation's data is found from its branch's root, never from
  its catalog entry, which is itself versioned. For hash-addressed engines
  (D1, D2), a persistent hash → block map locates nodes, and internal nodes
  carry an unhashed block hint per child so most reads skip the map.
- The page has two regions: the **canonical region**, whose bytes are hashed,
  and an **unhashed special space** (like nbtree's `BTPageOpaqueData`) for
  physical metadata: read hints, the cached hash, engine flags, and for D4 the
  version and lock data.
- Because a node must fit a page, the chunker must guarantee a split before
  the canonical region overflows. That forced split must itself be
  deterministic from content, so it is part of the canonical format.

### The chunker

- Designed for page-bounded nodes: target and minimum sizes are set relative to
  the page's usable space, chosen by measurement while building the first engine (roadmap step 4), then frozen.
- Boundary rule decided deliberately: whether it depends on keys only (stable
  boundaries under value updates, which keeps lock entries and SSI page
  mappings stable) or on bytes too (tighter size control). Open question 3.
- The boundary hash should be cheap and identical on every platform.
  Built (step 4a, `chunker_keyhash.c`): 64-bit FNV-1a over the key, seeded by
  level, with a splitmix64 finalizer, written out rather than taken from
  `hash_bytes`, whose output isn't specified across platforms.

### Content hashing

- Use a hash available in the Postgres tree where possible: SHA-256 via
  `src/common/cryptohash.c` (hardware-accelerated through OpenSSL when built
  with it), or add a faster one if measurement shows hashing dominates.
  Choosing it is a canonical-format decision (open question 4).

### Keys and values

- Keys: memcmp-comparable encodings generated from **btree opclasses and
  collations** (B6), so ordering matches what Postgres's planner and
  constraints already assume.
- Keys carry no local OIDs: encodings of arrays and composites of
  user-defined types name the type canonically (`09-versioned-catalogs.md`).
- Values: a canonical, portable row encoding (decided 2026-10-13, question 12
  changed): live columns only, in logical order, trailing NULLs omitted, enums
  by label, user types by canonical name, fixed byte order, no padding. Rows
  are decoded into Postgres's tuple layout with `TupleConversionMap`, so
  executor code still sees ordinary tuples. Postgres's on-disk tuple format
  was the first choice but depends on platform and history (dropped columns,
  catalog-supplied defaults, local OIDs inside values); `heaptuple` remains
  as an implementation for comparison.
- Large values: an out-of-line value tree within the same root, replacing
  TOAST for prolly tables, reusing Postgres's compression methods (pglz, lz4).

### Everything else through Postgres subsystems

- WAL: built-in resource managers (A3), written in Postgres's WAL style
  (`XLogBeginInsert` / `XLogRegisterBuffer` / `XLogInsert`), using
  full-page images for page-based engines exactly as heap does. Built in
  step 4b: `Prolly` (records from the log interface) and `ProllyStore`
  (the node store's own structures); each record names the implementation
  that wrote it.
- Locks: `XactLockTableWait`, multixacts, predicate.c (A4).
- Visibility: snapshots, procarray and commit publish (B1, B2).
- Schema: every catalog is a prolly table, versioned with the data; the
  working set is Postgres-native and version-control commits are canonical
  (`09-versioned-catalogs.md`).
- Branches, refs and the commit graph: exposed through SQL functions and
  procedures. Branch heads can't live in a versioned catalog; where they live
  is open question 32 (proposed: a WAL-logged refs file per database).

### Shared pieces every engine uses

- Table AM and index AM shells (`02-postgres-integration.md` A1, A2) over the
  internal API (section C there).
- Content-derived TIDs.
- **Canonical chunk export** and the cross-engine hash-equivalence test (see
  `05-storage-targets.md`).
- The schema canonicalizer, the working-set/staged/commit model, and the
  history-independence test (`09-versioned-catalogs.md`).
- The benchmark harness (`06-benchmarking.md`).

## Baseline

**B0: stock Postgres heap + nbtree.** Run with `full_page_writes` on and off so
the cost of torn-page protection is visible separately.

## The question that splits the engines

**Where do old row versions live for concurrent readers?** Copy-on-write
engines keep them in old roots that readers pin. An in-place engine cannot
overwrite a node a reader's root still reaches, so it must either edit only
unreachable nodes or keep versions inside the page as heap does. That splits
the coworker's in-place idea into D3 and D4.

## Engines

| | Pointers | Updates | Old versions for readers | WAL | Hashing | Layout |
|---|---|---|---|---|---|---|
| **D1 Immutable copy-on-write** | Hash | Always copy path | Pinned old roots | Full chunks; the chunk journal is the WAL | Every update | Append-only + GC |
| **D2 Copy-on-write, deferred writes** | Hash | Copy path in memory; dirty nodes in shared buffers | Pinned old roots | Logical row changes; chunks flushed at checkpoint | Deferred (checkpoint / Dolt commit) | Append-only + GC |
| **D3 Location-addressed, in place when unpinned** | Physical location; hash cached | In place if no pinned snapshot reaches the node, else copy | Pinned old roots | Full-page image + deltas | On demand (Dolt commit, diff, push) | Fixed-size blocks + allocation map |
| **D4 Location-addressed, versions in page** | Physical location; hash cached | Always in place | Row versions with xmin/xmax in an unhashed page section | Full-page image + deltas (like heap) | On demand, over committed content only | Fixed-size blocks + allocation map |
| **D5 Log-structured / disaggregated** (later) | Hash | Copy-on-write | Pinned old roots | Logical WAL is the source of truth; page servers build chunks | At the page server | Immutable objects |

### What each engine tests

- **D1: the cost of pure content addressing.** Dolt's storage model built on
  Postgres pages: the simplest engine and the reference point for the others.
- **D2: does a small synchronous logical WAL write plus deferred hashing beat
  D1?** This is the coworker's materialization question (axis 2).
- **D3: do in-place updates and an allocator pay off?** Two sub-variants for
  deciding a node is private:
  - **D3a birth epoch** (axis 1c): no counts.
  - **D3b reference counts** (axis 1b) with lazy background decrements.

  Expected to gain mainly on bulk writes and long transactions; short OLTP
  transactions rarely touch a node twice before it becomes visible.
- **D4: can heap's write path coexist with version control?** A heap-style
  page organized as a prolly tree. Brings vacuum back (dead in-page versions).
  Hashing must be computed over a committed-only view of each page. May allow
  real `xmin`/`xmax` values.
- **D5: the design for tiered/cloud storage.** Deferred until the storage
  target question (`05-storage-targets.md`) is settled.

## Coordination modules

| Module | What it is | Works with |
|---|---|---|
| **K1 Separate lock table** | Shared hash keyed by (relation, key) on the Postgres lock manager; coordination stays out of the tree | Any engine |
| **K2 Coordination plane on live nodes** | Option 8: unhashed, in-memory, per branch, with commit markers kept until the xmin horizon | D1, D2, D3 |
| **K3 Locks in the page** | xmax and lock bits in the page's unhashed section, as heap does | D3, D4; natural for D4 |

The coworker's point that coordination may need to span branches (axis 4b)
applies to the in-place engines (D3, D4) through per-chunk physical latches.
Measure it there.

## Pairings to build and measure

| Pairing | Isolates |
|---|---|
| **D1+K1** | Baseline cost of pure content addressing with the simplest locking |
| **D2+K1** | Logical WAL + deferred hashing vs D1 (axis 2), same locking |
| **D2+K2** | Where coordination lives (K1 vs K2), same engine |
| **D3+K2** | In-place updates vs copy-on-write (D3 vs D2), same coordination. Run as D3a and D3b |
| **D4+K3** | Heap-style in-page versions vs pinned roots |

## Settings within engines

| Setting | Values | Applies to |
|---|---|---|
| WAL form | full chunk / delta / logical, where the engine allows | All |
| Hashing | every update / deferred | D1, D2 (D3, D4 always on demand) |
| Read hints | on / off | D1, D2 (built in for D3, D4) |
| Block size | 512 B / 4 KB / 8 KB | D3, D4 |
| Group commit | Postgres's `commit_delay` / `commit_siblings` | All |
| Ordered commit pipeline | on / off | Copy-on-write engines with K2 |

Chunk target size and the chunker parameters are **not** settings. They are part
of the canonical format and must be identical everywhere
(`05-storage-targets.md`). They get chosen once by measurement and then frozen.

## Expected outcome

After measurement, probably two engines survive: one copy-on-write and one
in-place, selectable per storage type. The rest are experiments.
