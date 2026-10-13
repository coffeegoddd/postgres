# 08. Open questions

Nothing below is decided unless its entry says so. Update this file when something is.

## Status

| Topic | Status |
|---|---|
| Overall concurrency design | Open. Option 8 (coordination in the tree) was the user's stated favorite on 2026-10-12; the current plan is to measure candidates instead of choosing up front |
| Candidate set (D1–D5, K1–K3, pairings) | Proposed 2026-10-13; agreed as the working plan, not as final designs |
| Engine per storage type | Open; hypotheses in `05-storage-targets.md` |
| First engine and its version-control scope (13, 14) | Decided by the user 2026-10-13: D1+K1, V1–V3 |
| Row identity (2) | Decided for the first engine 2026-10-13: synthetic TIDs with a map |
| Node store (1) | Decided by the user 2026-10-13: one cluster-wide store, hash → block map plus per-child hints. Built (step 4b). 28–30 open |
| Chunk boundaries, content hash, own format (3, 4) | Starting choices for step 4 (user, 2026-10-13); built (4a); sizes still to be measured |
| Versioned schema (5, 21) | Decided by the user 2026-10-13: every catalog, shared ones included, is a prolly table |
| Canonical form, identity, value encoding (12, 5) | Decided by the user 2026-10-13: Postgres-native working set, canonical commits; identity by name; canonical schema derived from catalogs; canonical portable row encoding replaces `heaptuple` (question 12 changed). See `09-versioned-catalogs.md` part 1 |
| Root layout (33) | Decided by the user 2026-10-13: catalog sub-root and user-data sub-root. Local layout built 2026-10-13 (relations by OID, one entry per relation); the canonical layout by name comes with 4g |
| Caches, refs, schema merge, shared catalogs (23, 26, 27, 32, 34) | Proposals with agreed direction, `09-versioned-catalogs.md` part 2; open until built |
| Logical decoding (31) | Deferred by the user 2026-10-13 |

## Questions

1. **Node I/O (B4).** Starting assumption: nodes are `BLCKSZ` buffer-manager
   pages reached through smgr. Still to confirm: whether the forced split at a
   full page costs too much sharing, and whether D1/D2's hash → block map
   performs well enough, or a separate store with a checkpoint hook is needed.
   **Starting choice for the first engine (user, 2026-10-13):** nodes are
   pages in the table's main fork through the buffer manager, so no smgr
   patch. Still to be confirmed by measurement.
   **Superseded the same day (user):** versioned catalogs mean a relation's
   storage can't be found through, or die with, its catalog entry. Nodes
   live in **one cluster-wide node store**: every relation, database and
   branch, as buffer-manager pages of fixed relation file numbers in the
   global tablespace (no smgr patch). Branches point at roots in it;
   reclamation frees unreachable nodes. Children are found through a
   persistent **hash → block map**, plus an **unhashed per-child block
   hint** in internal nodes so most reads skip the map (user, 2026-10-13).
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
   with SHA-256 through `src/common/cryptohash.c`. The canonical format also
   covers schema, identity and row encoding: `09-versioned-catalogs.md`
   part 1.
5. **Catalogs.** Are system catalogs versioned per branch? Large and separate.
   **Decided 2026-10-13 (user):** the schema is versioned per branch, from the
   first engine on: a branch, commit, diff and merge carry table definitions
   as well as rows.
   **Decided 2026-10-13 (user): versioned catalogs.** All system catalogs,
   per-database and cluster-wide, are themselves prolly tables, so every catalog read goes through
   the session's branch root, and DDL writes into the branch. A branch's
   root set holds its catalog trees beside its user-table trees, which is
   Dolt's "schema in the root value", in Postgres's catalog form. Rejected
   alternatives: materializing a schema stored in the tree into heap
   catalogs on checkout, and an overlay on heap catalogs. Sub-questions are
   21–27 and 32–35 below.
   **Decided 2026-10-13 (user): how the Dolt promise is kept.** Catalogs hold
   Postgres-native rows (local OIDs); a canonical schema, identified by name
   and free of OIDs, is derived from them at version-control commit, diff,
   merge and transfer, and is what commits' root hashes cover. Details in
   `09-versioned-catalogs.md` part 1.
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
    **Changed the same day (user):** a canonical, portable row encoding
    replaces it as the default, because root hashes must not depend on
    history or platform: heap tuples keep dropped columns, rely on catalog
    defaults for added columns, and embed local OIDs in enum, array and
    composite values. Live columns only, logical order, trailing NULLs
    omitted, enums by label, user types by canonical name, fixed byte order,
    no padding; decoded into Postgres's tuple layout with
    `TupleConversionMap`. Rows untouched since a DROP COLUMN or a defaulted
    ADD COLUMN are rewritten at the next version-control commit. The
    `heaptuple` implementation stays registered, for comparison. Built
    2026-10-13 (`valenc_canonical.c`, `prolly_datum.c`).
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
    without the registry. For now (step 4b) the `Prolly` and `ProllyStore`
    resource managers have generic `desc`/`identify` in
    `rmgrdesc/prollydesc.c`: the implementation id and record type number,
    not the payload.
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
    `pg_authid`, `pg_tablespace`, `pg_shdepend`, …) are cluster-wide. Do
    they stay heap, and which per-database catalogs are versioned: all of
    them, or schema only (`pg_class`, `pg_attribute`, `pg_type`, `pg_index`,
    `pg_constraint`, `pg_attrdef`, `pg_namespace`, `pg_depend`, `pg_proc`,
    …) with `pg_statistic` and the like left per database?
    **Decided 2026-10-13 (user):** every catalog is versioned, per-database
    and cluster-wide (shared) alike. (Earlier the same day: shared catalogs
    stay heap; changed by the user.)
22. **Heap-specific catalog code.** Catalog writes go through
    `CatalogTupleInsert`/`Update`/`Delete` (`simple_heap_*`), in-place updates
    (`heap_inplace_update` for `relfrozenxid`, `relhasindex`, statistics),
    the relation mapper for mapped catalogs, and bootstrap mode in initdb.
    Each needs a table-AM path or a patch. Surveyed 2026-10-13: see
    `09-versioned-catalogs.md`.
23. **Caches per branch.** The syscache, relcache, catcache and plan cache
    must not serve one branch's definitions to a session on another.
    Per-branch caches, or invalidation on checkout and on root change?
    **Leaning (user, 2026-10-13):** per-branch caches. Not final until the
    cache design is worked out. Proposal (direction agreed 2026-10-13): a
    backend's caches reflect its branch by resetting on checkout;
    invalidation stays broadcast; separate cache sets per branch later if
    measured to matter (`09-versioned-catalogs.md` part 2).
24. **Code that reads catalog tuples' xmin/ctid.** Some code reads system
    columns of catalog tuples (for example to detect changed functions or
    for in-place updates). Those must work with synthetic TIDs and
    emulated xmin, or change. In-place updates themselves: proposed as
    unversioned side records (`09-versioned-catalogs.md` part 2).
25. **Heap user tables in a versioned database.** A heap table's definition
    would change with the branch while its data doesn't. Forbid heap tables
    in a versioned database, or treat their catalog rows as unversioned?
26. **Schema merge validity.** Merging catalog rows row by row can produce an
    inconsistent schema (a column added on one branch to a table dropped on
    the other; a dependency on a removed object). What checks run after a
    catalog merge, and how are conflicts reported? Proposal:
    `09-versioned-catalogs.md` part 2.
27. **Branch scope of shared catalogs.** Shared catalogs (`pg_database`,
    `pg_authid`, `pg_tablespace`, `pg_auth_members`, `pg_shdepend`,
    `pg_db_role_setting`, …) are read before a session has picked a
    database or branch: authentication reads `pg_authid`, and connection
    start reads `pg_database`. What branch do those reads see, and are
    shared catalogs branched per cluster, or per database alongside that
    database's catalogs? `pg_shdepend` links shared and per-database objects
    across those scopes.
    Options and proposal: `09-versioned-catalogs.md` part 2.
28. **Temporary and unlogged prolly tables.** The cluster-wide store is
    WAL-logged and in shared buffers. Temporary tables use backend-local
    buffers, and unlogged tables are reset after a crash. Do they get their
    own stores, stay heap, or aren't they supported?
29. **Tablespaces.** With one store, `CREATE TABLE … TABLESPACE` can't place a
    prolly table's data. One store per tablespace, or tablespaces ignored for
    prolly tables?
30. **One store under concurrency.** Every writer in the cluster extends the
    same relation files, so the relation extension lock and the map are
    shared hot spots. Bulk extension and map partitioning may be needed;
    measure in step 5.
31. **Logical decoding of prolly databases.** **Deferred (user,
    2026-10-13)** past the first engine. Approach notes in
    `09-versioned-catalogs.md` part 3, §7.
32. **Refs and bootstrap.** Branch heads can't be stored in a versioned
    catalog. Where are they, and how does startup find a branch's catalog
    trees before any catalog is readable? Proposal: a WAL-logged refs file
    per database, replaced atomically like `pg_filenode.map`
    (`09-versioned-catalogs.md` part 2).
33. **Root layout.** One flat map from relation identity to trees, or a
    catalog sub-root beside a user-data sub-root (so "schema changed?" is one
    hash comparison)? Needed for step 4c.
    **Decided 2026-10-13 (user):** two sub-roots. A branch's root points to
    a catalog sub-root (every catalog's trees) and a user-data sub-root (user
    tables' data and index trees). Schema changes are one hash comparison,
    diff can skip whichever side didn't change, and the canonical schema is
    derived from the catalog sub-root alone.
    **Built 2026-10-13 (user's choices):** the working-set (local) root first,
    keyed by relation OID, one entry per relation (a table and each index
    separately), behind its own interface (`root_layout`, implementation
    `subroots`). The canonical root uses the same shape keyed by name, with
    the canonicalizer (4g).
34. **Keyless catalogs.** `pg_depend` and `pg_shdepend` have no unique key.
    Key their trees by row content hash with a count (Dolt's keyless tables),
    or by a synthetic row id?
35. **Canonicalizer details.**
    - Expressions are stored as deparsed SQL, but `ruleutils.c` output can
      change between Postgres versions, which would change hashes on upgrade.
      Pin a canonical deparse format, or version it with the format stamp?
    - User data that holds OIDs (`oid` and `reg*` columns, large object
      references) is local by nature. Translate where the type says what it
      refers to (`regclass` → name), and refuse or pass through the rest?
    - Rename detection in diff: by matching content hashes, with what
      threshold for "same table, changed"?
