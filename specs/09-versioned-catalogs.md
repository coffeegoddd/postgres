# 09. Versioned schema and canonical form

**Goal (user, 2026-10-13):** be true to Postgres and true to Dolt. Schema is
versioned with data, a commit's root hash is determined by its logical content
alone (not by the operations that produced it), and clone and push work
between clusters without identifier collisions.

This file has three parts:

1. **The design** that meets that goal: decided 2026-10-13.
2. **Proposals** for the remaining mechanisms: the user agreed with the
   direction on 2026-10-13, but each is open until built and confirmed.
3. **The survey** of what Postgres assumes about catalogs being heap tables
   (2026-10-13), and what each assumption implies.

## 1. The design (decided 2026-10-13)

### Two layers: Postgres-native working set, canonical commits

- **The working set is Postgres-native.** Every system catalog, per-database
  and cluster-wide, is a prolly table on the branch (questions 5 and 21), with
  ordinary rows carrying local OIDs. syscache, relcache, the planner and DDL
  code work unchanged, and DDL keeps Postgres's speed: `ADD COLUMN … DEFAULT`
  stays instant.
- **Version-control operations work on canonical form:** version-control
  commit, diff, merge, clone, push and fetch. Canonical form has no OIDs, no
  physical layout, no history; it is portable between platforms and clusters.
  The hash promise is a promise about **commits**, as in Dolt.
- **Working set, staged, commit, as in Dolt.** A SQL `COMMIT` publishes a new
  working-set root for the branch; a version-control commit (`prolly_commit`)
  produces a commit with a canonical root. Staging follows Dolt's model.

### Identity by name

- Objects are identified in canonical form by name: a relation by qualified
  name, a function by qualified name and argument types, a type by qualified
  name, a role by name. This is what Dolt does for tables; random identifiers
  were rejected because two branches that independently create the same table
  must hash the same.
- A rename is a removal and an addition with identical content; diff reports
  it as a rename by matching content hashes (as git detects renames).
- OIDs are local to a cluster. Each cluster maps canonical identity to local
  OIDs; clone, fetch and merge assign local OIDs. When two branches each
  create `public.t`, merge treats them as the same table, as Dolt does.

### Canonical schema, derived from the catalogs

- Catalog trees hold Postgres-native rows (option "b"; storing canonical rows
  and translating on every catalog read was rejected).
- A **canonicalizer**, behind its own interface, derives the canonical schema
  from catalog rows:
  - drops local-only columns (the row's `oid`, `relfilenode`, statistics,
    `relfrozenxid`, …);
  - replaces OID references with canonical names, using the `BKI_LOOKUP`
    annotations Postgres already keeps on catalog columns (the metadata
    `genbki.pl` uses to turn names into OIDs at initdb);
  - stores expressions held as node trees (defaults, index expressions,
    constraints, views, policies) as SQL from Postgres's deparser
    (`ruleutils.c`).
- It runs at version-control commit, diff, merge and transfer, incrementally
  over changed catalog rows, not at every SQL commit.
- A commit records two roots: the **canonical root**, which is hashed and
  transferred, and the **local root** (catalog trees with local OIDs), which
  stays on the cluster so checking out an old commit is immediate. Clone and
  fetch rebuild local catalogs from the canonical form.

### Canonical rows

- Data trees are hashed as they are written (D1), so rows are stored in
  canonical form from the start. A canonical value encoding replaces the
  `heaptuple` encoding as the default (question 12, changed): live columns
  only, in logical order, trailing NULLs omitted, enums by label, user types
  inside arrays and composites by canonical name, fixed byte order, no
  padding. Rows are decoded into Postgres's tuple layout with the mapping
  Postgres already uses for dropped columns (`TupleConversionMap`).
- Key encodings follow the same rule: no local OIDs in key bytes.
- Rows written or updated are always canonical: the encoder skips dropped
  columns and fills in missing defaults. Rows untouched since a DROP COLUMN
  or an ADD COLUMN with a default are canonicalized at the next
  version-control commit, by rewriting the affected tables, so DDL stays
  instant and the cost is paid once, at commit, on tables whose layout
  changed.

### Scope and a correctness gate

- Canonicalizer coverage grows by object kind: tables, columns, types,
  constraints, indexes, defaults and sequences first; then views, functions,
  policies and the rest. A version-control commit touching an object kind not
  yet covered fails with a clear error; it never produces a hash that might
  depend on history.
- **History-independence gate** (`06-benchmarking.md`, gate 6): the same
  logical state reached through different histories (different DDL orders,
  insert/delete churn, different clusters) must give identical root hashes.
  A commit's own id also covers its parents and message, as in Dolt, so
  commit ids differ; `prolly_root_hash(ref)` returns the root hash.

### Not hashed

Statistics, `relfrozenxid`, relfilenodes, OIDs, and physical layout (block
hints, node locations).

## 2. Proposals (direction agreed 2026-10-13; open until built)

- **Root layout** (decided 2026-10-13, question 33). A branch's root points
  to two sub-roots: a **catalog sub-root**, mapping each catalog to its
  trees, and a **user-data sub-root**, mapping each user relation to its
  data and index trees. "Did the schema change?" is one hash comparison.
  Built for the working-set root (2026-10-13): relations by OID, one entry
  per relation, interface `root_layout` (`prolly_rootlayout.h`). Commits'
  canonical roots use the same shape keyed by qualified name (4g).
- **Catalog indexes are prolly index trees** in the same root (required:
  a heap btree index isn't versioned, so it would point at rows that don't
  exist on the branch). Each catalog's primary tree is keyed by its unique
  key; `pg_depend` and `pg_shdepend` have none, and are keyed by row
  content hash with a count, or by a synthetic row id (question 34).
- **Catalog snapshot** = the latest committed working-set root of the
  session's branch, refreshed when Postgres refreshes it today (on
  invalidation). A transaction's own catalog changes come from its private,
  command-id-tagged writes, the same mechanism as user tables (4e).
- **Caches:** checking out a branch resets the backend's caches, as a
  shared-invalidation overflow does today. Invalidation messages stay
  broadcast to every backend whatever its branch (extra invalidation is
  always safe). Relcache init files are keyed by the catalog root's hash.
  Per-branch cache sets (switching without a reset) are an optimization for
  later, if measured to matter (question 23).
- **DDL locks** stay global by OID (DDL on one branch blocks use of the table
  on another): safe and unchanged. Branch-qualified lock tags are a later
  option.
- **In-place updates** (statistics, `relfrozenxid`, survey §2) become
  unversioned side records, not catalog changes, so they never appear in
  diffs. Dolt doesn't version statistics either.
- **relfilenode** no longer locates data (the root does); it stays as a
  counter that changes on TRUNCATE and rewrites, for code that expects that.
- **Refs and bootstrap.** Branch heads can't live in a versioned catalog.
  A small refs file per database (branch names and root hashes), WAL-logged
  and replaced atomically like the relation mapper's `pg_filenode.map`, is
  read at startup to find the default branch's catalog trees. It replaces
  the nailed relations, `formrdesc` and the relation mapper as the way in
  (question 32).
- **Cluster-wide catalogs** (question 27): either cluster-level branches for
  shared catalogs, separate from per-database branches, or versioned history
  without branching. Carrying shared catalogs inside each database's branch
  conflicts with Postgres's startup order (`pg_database` is read before a
  database is chosen).
- **Schema merge:** three-way merge of catalog rows, then checks: catalog
  unique keys catch name collisions (two branches creating `public.foo`
  differently), `pg_depend` catches dependencies on removed objects, and
  rebuilding the relcache for every changed relation lets Postgres validate
  the merged definitions (question 26).
- **ADD and DROP COLUMN in merges:** Postgres's tuple layout tolerates
  schema evolution, so a branch that added a column merges with a branch
  that inserted rows without migrating rows. Changes that rewrite a table
  (`ALTER COLUMN TYPE`) against concurrent row changes are real conflicts, as
  in Dolt.

## 3. Survey: what Postgres assumes about heap catalogs

From a survey of the source on 2026-10-13. The mechanism for each item is
**not decided** unless it says so.

### Summary

Catalog **reads** are mostly table-AM neutral already: `systable_*` uses
slots, the catcache sits on `systable_*`, and `table_beginscan_catalog` is
generic. The work is in writes, side effects heapam performs on the
caller's behalf, in-place updates, code that uses physical TIDs and real
`xmin`, logical decoding, and the places where heap is hardcoded so that
catalogs can be opened before catalogs are readable.

| # | Area | Size | Difficulty |
|---|---|---|---|
| 1 | Catalog writes call heap directly | 6 functions in `indexing.c` (about 350 callers need no change), plus bootstrap and catalog TOAST | Medium |
| 2 | In-place updates and their tuple-lock interlock | 6 caller groups (3 on `pg_class`, 3 on shared `pg_database`), about 30 tuple-lock sites | Large |
| 3 | Reads that bypass the table AM | 34 `heap_getnext` loops (19 per-database, 15 shared); 1 raw heap-page reader (`CREATE DATABASE … STRATEGY WAL_LOG`) | Small (loops), medium (the rest) |
| 4 | Code that reads catalog tuples' `xmin`/`ctid` | About 12 sites | Medium |
| 5 | Heap hardcoded for bootstrap and nailed catalogs | About 10 sites | Small–medium |
| 6 | Relation mapper | Generic; mapped catalogs must open before `pg_class` | Small |
| 7 | Horizons, freezing, logical decoding | Historic MVCC for catalogs is the hardest single item | Large |
| 8 | CLUSTER/VACUUM FULL/REINDEX on catalogs, catalog TOAST, pg_upgrade, amcheck | About 15 sites | Medium |
| 9 | Shared catalogs read before a database is chosen | About 25 sites | Medium |

### Details and what they imply

#### 1. Writes

- `CatalogTupleInsert`/`Update`/`Delete`, `CatalogTupleInsertWithInfo`,
  `CatalogTupleUpdateWithInfo` and `CatalogTuplesMultiInsertWithInfo`
  (`src/backend/catalog/indexing.c`) call `simple_heap_*` and
  `heap_multi_insert`. Changing them to the table AM (`table_tuple_insert`
  and friends, through slots) fixes all callers at once.
- `CatalogIndexInsert` skips index inserts for heap-only (HOT) tuples and
  uses `t_self` as the index TID, so the AM must set `t_self` to the row's
  TID after insert and update. About 260 callers pass `&tup->t_self` back as
  the old TID, so TIDs from scans must round-trip into update and delete.
- **Side effects inside heapam that the AM must reproduce:** cache
  invalidation (`CacheInvalidateHeapTuple`, for per-database and shared
  catalogs alike) and, under `wal_level = logical`, `XLOG_HEAP2_NEW_CID`
  records for logical decoding.
- Also direct: bootstrap's `InsertOneTuple` (`bootstrap.c`), how initdb loads
  every catalog row; and catalog TOAST writes (`toast_internals.c`).

#### 2. In-place updates

- `systable_inplace_update_begin/finish/cancel` (`genam.c`) assume a buffer
  heap tuple slot and call `heap_inplace_lock`, `heap_inplace_update_and_unlock`
  and `heap_inplace_unlock`, which overwrite a tuple's bytes without a new
  version.
- Callers: `vac_update_relstats` and `index_update_stats` (pg_class: relpages,
  reltuples, relallvisible, relfrozenxid, relminmxid, relhasindex), toasting
  during bootstrap (pg_class), and on shared `pg_database`:
  `vac_update_datfrozenxid`, DROP DATABASE marking, and event triggers
  (`dathasloginevt`).
- A tuple-lock interlock (`SearchSysCacheLocked1`, `LockTuple(…,
  InplaceUpdateTupleLock)`, about 30 sites) keeps ordinary updates from losing
  an in-place write. It locks by TID, so synthetic TIDs must be stable for the
  life of a row version.
- **Implication:** an in-place write is a non-versioned change. Options: a
  table-AM in-place callback that the prolly AM implements as a
  non-versioned side record; or making these ordinary transactional updates,
  which changes their semantics (and, with versioning, means statistics and
  frozen-xid values would differ by branch and show up in diffs). Proposed:
  unversioned side records (part 2).

#### 3. Reads that bypass the AM

- `heap_getnext` raises an error for any non-heap AM; 34 catalog scan loops
  use it and must become `table_scan_getnextslot`.
- `systable_getnext` calls `ExecFetchSlotHeapTuple` and asserts the slot
  owns the tuple, so the prolly AM's catalog slots must hand out HeapTuples
  (heap-tuple slots) with `t_self` and `t_tableOid` set.
- Catalog scans use MVCC catalog snapshots, `SnapshotAny`, `SnapshotSelf`,
  `SnapshotDirty` (unique checks on catalog indexes) and historic MVCC (§7).
- `CREATE DATABASE … STRATEGY WAL_LOG` reads the template's `pg_class` pages
  raw (`dbcommands.c`, `ScanSourceDatabasePgClass`).
  **Implication:** with one cluster-wide node store (question 1), creating a
  database from a template can be a copy of the template's root set, with no
  file copying.

#### 4. `xmin` and `ctid` of catalog tuples

- Function caches decide whether a cached function is current by `pg_proc`
  `xmin` and TID: `funccache.c` (PL/pgSQL, SQL functions), `fmgr.c` (C
  functions), PL/Perl, PL/Tcl.
- `indcheckxmin`: `pg_index` `xmin` against `TransactionXmin`
  (`relcache.c`, `plancat.c`).
- Also `pg_inherits` (detach pending), `pg_enum` (safe-enum check), logical
  replication conflict detection, and `heap_getsysattr` for `SELECT xmin
  FROM pg_class`.
- **Implication:** the AM must supply a per-version identity that changes
  whenever a row version changes (an emulated `xmin`, or a version token
  derived from the content hash), or these sites move to invalidation
  callbacks (question 24).

#### 5–6. Bootstrap, nailed catalogs, relation mapper

- Heap is hardcoded: `bootparse.y` (every catalog created with
  `HEAP_TABLE_AM_OID`), `pg_class.h` (`relam` default), `formrdesc` and
  `RelationInitTableAccessMethod` in `relcache.c` (nailed catalogs get the heap
  handler without a syscache lookup), and a few checks elsewhere.
- Nailed catalogs (`pg_class`, `pg_attribute`, `pg_proc`, `pg_type`, and six
  shared ones) are opened before any catalog can be read. The prolly AM must
  be reachable the same way: hardcoded handler, no catalog lookups to open.
- The relation mapper maps OIDs to relfilenumbers and is otherwise generic.
  With one node store, a catalog's data is found from its branch's root, not
  its relfilenumber; the mapper would still name catalogs, but VACUUM FULL
  and CLUSTER on catalogs would publish a new root rather than swap files.

#### 7. Horizons, freezing, logical decoding

- Catalog row versions must be kept until `catalog_xmin` of replication
  slots (procarray horizons), or logical decoding and standby catalog reads
  break. Reclamation (step 4f) must honour these horizons.
- Freezing: an AM without xids should report an invalid `relfrozenxid`;
  wraparound and autovacuum logic must tolerate that for catalogs.
- **Historic MVCC (hardest):** logical decoding reads catalogs as of a past
  point, including a decoded transaction's own uncommitted catalog changes by
  command id (`HeapTupleSatisfiesHistoricMVCC`, cmin/cmax maps keyed by
  ctid from `XLOG_HEAP2_NEW_CID`, rewrite mappings for catalog rewrites).
  With immutable roots, "catalogs as of a past commit" is a root lookup;
  the in-transaction, per-command-id view is the hard part.
- **Deferred (user, 2026-10-13):** logical decoding of prolly databases is
  out of scope for the first engine; nothing in roadmap steps 4–6 needs it.
  Notes for when it is taken up:
  - Postgres keeps old catalog row versions until a slot's `catalog_xmin`,
    builds historic snapshots from the WAL (`snapbuild.c`), replays each
    transaction at commit under that snapshot so ordinary syscache lookups
    see the catalogs as they were, and resolves in-transaction visibility
    from `XLOG_HEAP2_NEW_CID` records (ctid → cmin/cmax). The uncommitted
    catalog rows are already in shared heap pages; only command ids come
    from the WAL.
  - Prolly analogue (proposed): keep committed roots until `catalog_xmin`
    passes; a historic snapshot is the committed root as of the LSN.
    Uncommitted changes live in the writer's private state, so under
    `wal_level = logical` they would be logged with command ids and replayed
    by the decoder as a per-transaction overlay on the committed root.
  - Separately, decoding prolly table data needs row-level change records
    under `wal_level = logical`, since D1 logs whole nodes.

#### 8. Other

- CLUSTER/VACUUM FULL/REPACK and REINDEX on catalogs have catalog-specific
  paths (`repack.c`, `heapam_handler.c`), including tolerating in-progress
  tuples during catalog index builds.
- About 40 catalog TOAST tables (shared ones included).
- pg_upgrade copies some catalog files physically and freezes catalogs;
  amcheck, pageinspect, pgstattuple and pg_visibility are heap-only.

#### 9. Shared catalogs before a database is chosen

- Connection start (`postinit.c`) reads `pg_database` and authentication
  reads `pg_authid`/`pg_auth_members`/`pg_db_role_setting` before
  `MyDatabaseId` is set, from nailed shared relcache entries or the shared
  relcache init file (`global/pg_internal.init`).
- The launcher processes (logical replication, autovacuum) and the data
  checksums worker read shared catalogs with no database.
- Shared catalog invalidations are broadcast with `InvalidOid` as the
  database.
- **Implication:** the AM must work with no database selected, and reads of
  shared catalogs need a branch to read from before a session has chosen one
  (question 27). A cluster-wide node store (question 1) fits this; the branch
  question is open (part 2).

### Consequences already taken into account

- One cluster-wide node store (question 1): shared catalogs need storage
  reachable without a database, and `CREATE DATABASE` becomes a root-set
  copy.
- Caches reflect the session's branch (question 23; part 2).
- Order: versioned catalogs need a complete engine (concurrency, WAL,
  recovery), so they come in step 4g, with the canonicalizer and the
  working-set/commit model.
