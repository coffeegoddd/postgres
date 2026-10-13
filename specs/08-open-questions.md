# 08. Open questions

Nothing below is decided unless its entry says so. Update this file when something is.

## Status

| Topic | Status |
|---|---|
| Overall concurrency design | Open. Option 8 (coordination in the tree) was the user's stated favorite on 2026-10-12; the current plan is to measure candidates instead of choosing up front |
| Candidate set (D1–D5, K1–K3, pairings) | Proposed 2026-10-13; agreed as the working plan, not as final designs |
| Engine per storage type | Open; hypotheses in `05-storage-targets.md` |
| First engine, its VC scope, row identity, value encoding, versioned schema (2, 5, 12, 13, 14) | Decided by the user 2026-10-13 for step 4; see each question |
| Versioned schema mechanism (5, 21) | Decided by the user 2026-10-13: per-database catalogs are prolly tables, shared catalogs stay heap; 22–26 open, leaning to per-branch caches (23) |
| Node I/O, chunk boundaries, canonical format (1, 3, 4) | Starting choices for step 4 (user, 2026-10-13); can change |

## Questions

1. **Node I/O (B4).** Starting assumption: nodes are `BLCKSZ` buffer-manager
   pages reached through smgr. Still to confirm: whether the forced split at a
   full page costs too much sharing, and whether D1/D2's hash → block map
   performs well enough, or a separate store with a checkpoint hook is needed.
   **Starting choice for the first engine (user, 2026-10-13):** nodes are
   pages in the table's main fork through the buffer manager, so no smgr
   patch. Still to be confirmed by measurement.
2. **Row identity (B7).** Synthetic content-derived TIDs with a map, or patch
   core for wide row identifiers?
   **Decided 2026-10-13 for the first engine:** synthetic TIDs with a map, no
   core patch. It's an implementation of the row_identity interface, so it can
   be replaced.
3. **Chunker boundary rule.** Boundaries that depend on keys only stay put when
   values change, so K2's lock entries and SSI page mappings stay stable. Rules
   that also count bytes control node size better. Page-bounded nodes need a
   forced split either way. Which rule, and what size targets?
   **Starting choice for the first engine (user, 2026-10-13):** boundaries
   depend on keys only, plus a forced split when the page is full. Size
   targets are set by measurement in step 4.
4. **Canonical format.** Our own Postgres-native format (node layout, key and
   value encoding, content hash), or Dolt byte compatibility for DoltHub
   interop? Dolt compatibility would constrain nearly every foundation choice,
   so the default is our own format.
   **Starting choice for the first engine (user, 2026-10-13):** our own format,
   with SHA-256 through `src/common/cryptohash.c`.
5. **Catalogs.** Are system catalogs versioned per branch? Large and separate.
   **Decided 2026-10-13 (user):** the schema is versioned per branch, from the
   first engine on: a branch, commit, diff and merge carry table definitions
   as well as rows.
   **Decided 2026-10-13 (user): versioned catalogs.** The database's system
   catalogs are themselves prolly tables, so every catalog read goes through
   the session's branch root, and DDL writes into the branch. A branch's
   root set holds its catalog trees beside its user-table trees, which is
   Dolt's "schema in the root value", in Postgres's catalog form. Rejected
   alternatives: materializing a schema stored in the tree into heap
   catalogs on checkout, and an overlay on heap catalogs. Sub-questions are
   21–26 below.
6. **Reference counting (D3b).** Can decrements be propagated lazily without
   unbounded backlog after a branch delete? What bounds the background work?
7. **Cross-branch coordination (axis 4b).** Is per-chunk latching across
   branches needed for in-place engines, and what does it cost?
8. **D4 canonicalization.** How is a committed-only view of an in-place page
   computed cheaply and consistently for hashing?
9. **Commit markers (K2).** Memory bound under long-running snapshots; what to
   do at the limit (spill, or error like `max_locks_per_transaction`)?
10. **Decision rule.** What margin versus B0 on the S suite is acceptable in
    exchange for version control? (For after the roadmap's step 6.)
11. **Storage targets in scope.** Is cloud/tiered storage (D5) a goal, or is
    local SSD/NVMe enough for now?
12. **Value encoding.** Reuse Postgres's tuple format so executor deforming
    works unchanged, or define a portable canonical row encoding (fixed
    endianness, no alignment padding) and deform from it? Canonical chunks must
    be identical across platforms for clone/push/pull.
    **Decided 2026-10-13 for the first engine:** Postgres's tuple format. It's
    an implementation of the value_encoder interface; a portable encoding can
    replace it when clone/push between architectures matters.
13. **First engine.** D1+K1 is proposed for roadmap step 4 because it is the
    simplest and the reference point for the others. Confirm before starting.
    **Decided 2026-10-13:** D1+K1.
14. **Version-control scope for the first engine.** Which of V1–V4 must D1+K1
    support in step 4? Clone/push/fetch (V4) needs a transfer protocol, which
    may be more than the first engine needs.
    **Decided 2026-10-13:** V1–V3 (branch, checkout, commit, diff, merge on one
    cluster). V4 (clone, fetch, push) stays a stub that reports not
    implemented.
15. **Choosing a pairing per table.** Today the table AM name picks a fixed
    profile (`prolly_d2` → D2+K1). D2+K2 needs another way to pick K2: another
    AM name, or a setting read only at table creation and recorded in the
    tree (so a session can never read a table with different components than
    wrote it). Needed before D2+K2 is built.
16. **WAL description in `pg_waldump`.** The log interface's `desc` and
    `identify` must also run in `pg_waldump`, which is a frontend program
    without the registry.
17. **Index trees on non-prolly tables.** `CREATE INDEX … USING prolly` on a
    heap table is accepted up to `ambuild` today. Should prolly indexes be
    refused on heap tables, or supported?
18. **Loading the larger-than-RAM size.** A fresh ~300 GB load per cell is
    impractical (5 repetitions x 5 client counts x 9 workloads). Options: load
    once per workload and size and reuse it across cells (results then depend
    on cell order), or keep a loaded copy and restore it per cell (a 300 GB
    copy each time, since the data directory is copy-on-write-free). Needed
    before step 3 runs that size.
19. **Indexes on prolly tables in the S suite.** pgbench and BenchBase create
    their primary keys and indexes as btree. On a prolly table that means a
    btree index over a prolly table, not a prolly index tree. Should the S
    suite make prolly the default index AM for prolly runs (no such setting
    exists in Postgres; it would need a small core change or rewriting the
    workloads' DDL)?
20. **Device bytes on this machine are dominated by btrfs fsync.** In the
    step 2 smoke run every write workload wrote 45-77 KB to the device per
    transaction, whatever its WAL volume (S5: 127 bytes of WAL, 46 KB on the
    device; read-only workloads: ~0). That is a fixed cost per commit fsync
    from the filesystem (most likely btrfs's fsync log tree), not from
    Postgres, and it would swamp the engine differences the metric is meant
    to show. Options for step 3: run the data directory on a dedicated
    ext4/XFS partition or device, or report device bytes with this caveat.
    Not yet confirmed with a filesystem-level trace.
    **Decided for now (2026-10-13):** no separate partition or device. Runs
    stay on the btrfs `/home` with the data directory copy-on-write-free;
    device bytes are reported with this caveat, and WAL bytes per
    transaction are the cleaner measure of write volume.
21. **Which catalogs are versioned.** Shared catalogs (`pg_database`,
    `pg_authid`, `pg_tablespace`, `pg_shdepend`, …) are cluster-wide and
    presumably stay heap. Which per-database catalogs are versioned: all of
    them, or schema only (`pg_class`, `pg_attribute`, `pg_type`, `pg_index`,
    `pg_constraint`, `pg_attrdef`, `pg_namespace`, `pg_depend`, `pg_proc`,
    …) with `pg_statistic` and the like left per database?
    **Decided 2026-10-13 (user):** cluster-wide (shared) catalogs stay heap;
    every per-database catalog is versioned.
22. **Heap-specific catalog code.** Catalog writes go through
    `CatalogTupleInsert`/`Update`/`Delete` (`simple_heap_*`), in-place updates
    (`heap_inplace_update` for `relfrozenxid`, `relhasindex`, statistics),
    the relation mapper for mapped catalogs, and bootstrap mode in initdb.
    Each needs a table-AM path or a patch. The full list has to be found
    before estimating the work.
23. **Caches per branch.** The syscache, relcache, catcache and plan cache
    must not serve one branch's definitions to a session on another.
    Per-branch caches, or invalidation on checkout and on root change?
    **Leaning (user, 2026-10-13):** per-branch caches. Not final until the
    cache design is worked out.
24. **Code that reads catalog tuples' xmin/ctid.** Some code reads system
    columns of catalog tuples (for example to detect changed functions or
    for in-place updates). Those must work with synthetic TIDs and
    emulated xmin, or change.
25. **Heap user tables in a versioned database.** A heap table's definition
    would change with the branch while its data doesn't. Forbid heap tables
    in a versioned database, or treat their catalog rows as unversioned?
26. **Schema merge validity.** Merging catalog rows row by row can produce an
    inconsistent schema (a column added on one branch to a table dropped on
    the other; a dependency on a removed object). What checks run after a
    catalog merge, and how are conflicts reported?
