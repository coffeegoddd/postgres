# 09. Versioned catalogs

**Decided (user, 2026-10-13):** every system catalog, per-database and
cluster-wide, is a prolly table, so schema is versioned with data
(`08-open-questions.md`, questions 5 and 21). Catalog reads go through the
session's branch root; DDL writes into the branch. This file records what
Postgres assumes about catalogs being heap, from a survey of the source on
2026-10-13, and what each assumption means for the design. The mechanism for
each item is **not decided** unless it says so.

## Summary

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

## Details and what they imply

### 1. Writes

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

### 2. In-place updates

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
  frozen-xid values would differ by branch and show up in diffs).

### 3. Reads that bypass the AM

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

### 4. `xmin` and `ctid` of catalog tuples

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
  callbacks.

### 5–6. Bootstrap, nailed catalogs, relation mapper

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

### 7. Horizons, freezing, logical decoding

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

### 8. Other

- CLUSTER/VACUUM FULL/REPACK and REINDEX on catalogs have catalog-specific
  paths (`repack.c`, `heapam_handler.c`), including tolerating in-progress
  tuples during catalog index builds.
- About 40 catalog TOAST tables (shared ones included).
- pg_upgrade copies some catalog files physically and freezes catalogs;
  amcheck, pageinspect, pgstattuple and pg_visibility are heap-only.

### 9. Shared catalogs before a database is chosen

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
  question does not have an answer yet.

## Consequences already taken into account

- One cluster-wide node store (question 1): shared catalogs need storage
  reachable without a database, and `CREATE DATABASE` becomes a root-set
  copy.
- Per-branch caches (question 23, leaning).
- Order: versioned catalogs need a complete engine (concurrency, WAL,
  recovery), so they come in step 4g.
