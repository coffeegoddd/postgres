# 08. Open questions

Nothing below is decided unless its entry says so. Update this file when something is.

## Status

Since 2026-10-15 the design being built is `11-pages-and-dolt-snapshots.md`
(Postgres pages for the working set, a chunk store for history). Rows
marked **(D1)** are about the tree engine D1 and its components, kept as
the record of what was decided for it. D1 is removed by #177, #178 and
#183; its last state is `cff08be5c3`.

| Topic | Status |
|---|---|
| Overall concurrency design | **Superseded** by the direction (2026-10-15): the working set is Postgres pages, with Postgres's MVCC and locks (locks per branch, #91). Was: open. Option 8 (coordination in the tree) was the user's stated favorite on 2026-10-12; the current plan is to measure candidates instead of choosing up front |
| Candidate set (D1–D5, K1–K3, pairings) | **(D1)** Superseded by the direction (2026-10-15); D1 removed by #183 (row "Retiring D1"). Was: Proposed 2026-10-13; agreed as the working plan, not as final designs |
| Engine per storage type | **Superseded** by the direction (2026-10-15): one design, pages plus a chunk store. Was: open; hypotheses in `05-storage-targets.md` |
| First engine and its version-control scope (13, 14) | Decided by the user 2026-10-13: D1+K1, V1–V3 |
| Row identity (2) | **(D1)** Decided for the first engine 2026-10-13: synthetic TIDs with a map |
| Node store (1) | **(D1)** Replaced for the page design by the chunk store (step 3, #92, #110). Decided by the user 2026-10-13: one cluster-wide store, hash → block map plus per-child hints. Built (step 4b). 28–30 open |
| Chunk boundaries, content hash, own format (3, 4) | Starting choices for step 4 (user, 2026-10-13); built (4a); direction for the chunk rule and page packing set (user, 2026-10-14), settled by measurement in step 5 |
| Versioned schema (5, 21) | **Superseded** by versioned schemas (user, 2026-10-16; row "What is versioned"). Was: Decided by the user 2026-10-13: every catalog, shared ones included, is a prolly table |
| Canonical form, identity, value encoding (12, 5) | Decided by the user 2026-10-13: Postgres-native working set, canonical commits; identity by name; canonical schema derived from catalogs; canonical portable row encoding replaces `heaptuple` (question 12 changed). See `09-versioned-catalogs.md` part 1 |
| Root layout (33) | **(D1)** Decided by the user 2026-10-13: catalog sub-root and user-data sub-root. Local layout built 2026-10-13; changed 2026-10-14 (user): local roots key relations by relfilenumber and add a third, local-only sub-root for TID maps. The canonical layout by name comes with 4g |
| Table keying, TID maps (36, 37) | **(D1)** For the page design, keyless rows are keyed otherwise since #149 (row "Keyless keying"); no TID maps. Decided by the user 2026-10-14: primary key if present (an immediately checked one; deferrable ones key by content, 4d.2), else row content hash with a count; TID map as a per-relation tree in the local sub-root. Built in 4d.1 |
| Caches, refs, schema merge, shared catalogs (23, 26, 27, 32, 34) | Proposals with agreed direction, `09-versioned-catalogs.md` part 2; open until built |
| Row locks, update chains (38, 39) | **(D1)** Decided by the user 2026-10-14: per-table writer lock, exclusive for writes and FOR [NO KEY] UPDATE, shared for FOR [KEY] SHARE; forwarding entries in the TID map. Built in 4d.2. The writer lock was a stopgap: K1's row-level locks replaced it in 4e.3 |
| Prolly indexes (17, 42, 43) | **(D1)** Decided by the user 2026-10-14: refused on heap tables; entries point at row keys; the table AM removes old entries at once. Built in 4d.3 |
| ANALYZE sampling (44) | **(D1)** Decided by the user 2026-10-14: a table AM hook for the row sampler, and positional lookups from subtree counts; keyless tables and tables the transaction wrote are read whole. Built in 4d.4 |
| Canonical format freeze, transaction memory, large values, reclamation roots (45–49) | For the page design: the format frozen (#114) and changed four times since, each a clean break of the root's `"!:format"` item (`FOLDED_FORMAT`): keyless keying (#149, "2"), names escaped in the canonical text (#195, "3"), value trees addressed by byte counts (#218, "4") and a row's long values each a value tree of its own (#219, "5"); transaction memory is Postgres's; reclamation is step 7. Was: Recommended 2026-10-14 after the Dolt and DoltLite comparisons, scheduled in 4e–4g (roadmap); open until built |
| TID source, reuse, clone (37 changed, 40, 41) | **(D1)** Decided by the user 2026-10-14: per-table counter TIDs, built in 4d.2; reuse after VACUUM required in 4f; implicit maps planned for clone |
| Temporary and unlogged tables (28) | Decided by the user 2026-10-14: heap, never versioned, even when prolly is the default; an explicit `USING prolly` on them is refused. Built in 4e.5 |
| Step 4's remaining decisions (23, 25, 27, 29, 34, 35, 46–56) | Decided by the user 2026-10-14, as recommended: see each question |
| Logical decoding (31) | Deferred by the user 2026-10-13 |
| Direction (2026-10-15) | Decided by the user 2026-10-15: Postgres pages for the working set, copy-on-write page sets per branch, a Dolt-shaped chunk store for history (`11-pages-and-dolt-snapshots.md`). Supersedes prolly trees as primary storage; D1 tagged `prolly-d1`. Steps 0–5 built 2026-10-17 (below) |
| Branch copies' OIDs (2026-10-16) | Decided by the user: a branch's copy of a versioned object keeps its source's OIDs; catalog rows carry a branch, and a backend sees one branch (`11-pages-and-dolt-snapshots.md`, part 1) |
| Spec 11, steps 0–5 (2026-10-17) | Built and merged, with the decisions recorded there (steps 2–5's in #90). Step 6's file remotes built (#171, #172, #174, #175); its network remotes, and steps 7–8, are next, with their decisions recorded in spec 11 |
| What is versioned (2026-10-16) | Decided by the user: versioned schemas (`CREATE SCHEMA … VERSIONED`), every object in them per branch, Doltgres's scope; prolly tables only there; dependencies only from versioned objects to shared ones (`11-pages-and-dolt-snapshots.md`, part 1) |
| Keyless keying (2026-10-17) | Decided by the user: a keyless row is keyed by the first 32 bytes of its encoded values, then its hash (#149), a clean break of format: a folded root records its format ("!:format"), and an older root's tables' rows aren't read (`13-keyless-tables.md`) |
| Keyless merge rule (2026-10-17) | Decided by the user: the same change on both sides is made once; otherwise copies added on either side add up and copies removed on both are removed once; a keyless row never conflicts (#150) |
| Escaped canonical text (2026-10-17) | Decided by the user: a name or expression holding a tab, newline or backslash is escaped in the canonical text, a clean break (format "3" of `"!:format"`, #195); refusing such names was the alternative, rejected as they are legal Postgres. A root of an older format has its rows unread and is neither moved to nor merged (`11-pages-and-dolt-snapshots.md` step 4's notes) |
| Remotes (step 6) | Decided by the user 2026-10-17, as recommended: libpq transport, clone into an empty database, a push refused over uncommitted changes, predefined roles for fetch and push, object storage later, the transfer from known heads with the first network remote (`11-pages-and-dolt-snapshots.md` part 7). File remotes built (#171, #172, #174, #175); network remotes, batching, the transfer from known heads, a push's dependency check and V4 built and merged (#198–#203, `11-pages-and-dolt-snapshots.md` 7.12). Not built: a client's branch deletion (`--prune`), resuming, a bounded seen set, the whole-file clone |
| Clone's shared-dependency check (2026-10-17) | Decided by the user: commits record outside dependencies (extensions with versions, ordinary-schema objects; not roles, user 2026-10-17, since owners and grants aren't versioned, as in Postgres's dumps and Dolt and Doltgres) in a tree the commit names beside its root, not in the root (user 2026-10-17), so outside changes never make a branch dirty; clone checks every branch's dependencies before creating anything, naming all missing, then creates the extensions the database lacks at the newest version any branch recorded; moves never change extensions, refusing instead; extensions are not versioned per branch (an upgrade is tested in a clone) (`11-pages-and-dolt-snapshots.md` 7.4). Built: #188 (recorded), #189 (clone), #192 (moves). Not as decided: diffs don't show dependencies (they are beside the root, which diffs compare); not built |
| Retiring D1 (2026-10-17) | Decided by the user: D1 is removed after file remotes merge, before network remotes, not after step 8 (which builds D1 from its last state, `cff08be5c3`); D1-only test coverage ported first; interfaces no remaining component uses deleted with D1, every one this design uses kept (`11-pages-and-dolt-snapshots.md` part 4). Done in #177 (coverage ported to versioned schemas), #178 (tests and harness) and #183 (code) |
| Reclamation (step 7) | Decided by the user 2026-10-17, as recommended: online GC by seal, horizon and an append check on every batch; default and full generations; automatic by autovacuum, on; unreachable commits collected; the page sweep holds off branch creation (`11-pages-and-dolt-snapshots.md` part 8). Built and merged in #210, #211, #212, #213, #214, #216, #223 and #225; where it departs from the design: part 8.11 (the append check covers the references the tree format declares, other hashes only against what a swap dropped, so the closure is weaker than every reference looked up; conservative marking; counts past 255 not kept apart; the sweep's trigger; no automatic full collection) |
| Interfaces under the pages | **Deferred** (user, 2026-10-17): only smgr is an interface (step 1); the buffer manager, WAL, checksums, AIO and read streams and `pg_stat_io` stay Postgres's until step 8's measurements show another implementation would pay. Was: open (user, 2026-10-15, "not sure yet") |
| The work up to step 8 (2026-10-17) | Decided by the user, each as recommended (`11-pages-and-dolt-snapshots.md` part 4, "Up to step 8"), in order: the dependency check at moves; names escaped in the canonical encoding, a clean break (every format change before step 8 is one); network remotes; reclamation; aggregates, policies and partitioned tables held by commits, foreign tables still refused (with the readers of history refusing a table whose row security applies to them); large values: the `oid` refusal, then spec 14's B, with C after measurement (jsonb's canonical bytes on-disk with a version byte, the pointer tagging its hash algorithm); the correctness suite in CI only once its baseline is clean. Built and merged by 2026-10-18, but the correctness suite, which isn't in CI yet (its baseline isn't clean) (`11-pages-and-dolt-snapshots.md` part 4); since (#230, #231): range types, extended statistics, casts and collations of versioned schemas are held, every other kind a commit doesn't hold is refused instead of left out, and only a commit refuses (a move discards them, a checkout leaves them on their branch) |
| Complexity of version-control operations | Required by the user 2026-10-14: at worst Dolt's time and space bounds, ideally better (`10-complexity-requirements.md`, gate 7) |
| Steps 2–5's remaining decisions (2026-10-16) | Decided by the user: branch-aware locks; branch privileges; LZ4 table files; collation versions refused on mismatch; `dolt_commit` inside its transaction; checkpoints on request and branch switch; format freeze at the end of step 4; keyless O(N) diff fallback; Dolt's working-set behavior for reset and merge; no triggers on applied rows; Dolt's schema merge; read-only detached commits; sequences merged to the larger value (`11-pages-and-dolt-snapshots.md`, steps 2–5) |
| SQL correctness suite (C) | Decided by the user 2026-10-15: sqllogictest from `prolly_bench`, judged by agreement with heap; `prolly` runs it in a versioned schema; engines heap and `prolly` (`12-correctness-suite.md`, gate 8) |
| Dolt test parity scope (#164) | Decided by the user: every Dolt version-control feature is in scope except the staging area (`dolt_workspace_*`, `dolt_rm`, soft and mixed reset, `--cached`); MySQL-only syntax and types and Dolt's storage internals aren't ported (`18-dolt-divergences.md`) |
| Round 1 of the issue queue (2026-10-18) | Decided by the user, each as recommended. **Writer modes** (#148): "dirty" is "has an owner", set at the claim and cleared by `dolt_commit`, a hard reset or an integration commit; the mode stored per database and changed by a `dolt_*` call that takes owning the database; `branch-auto` switching at the start of the first writing statement; a push, fetch or `dolt_branch` moving a protected branch allowed for the database's owner (`11-pages-and-dolt-snapshots.md` 6.2, 7.6). **Transaction commits** (#228, #229): a commit per autocommitted statement; conflicts with `dolt.allow_commit_conflicts` commit the SQL transaction without a Dolt commit, with a warning; objects a commit can't hold fail `COMMIT`; phase 2 doesn't wait for phase 1's numbers (`15-transaction-commits.md` part 8). **Test parity** (#153): ports of what prolly runs now plus small features (revert `--abort`/`--continue`, merge `--ff-only`/`--squash`/`--no-commit`, `dolt_branch -m`, `dolt_merge_base()`, `dolt_verify_constraints()` beyond foreign keys), large features as issues of their own; a classification and coverage report, ports by hand; divergences fixed to match Dolt unless Postgres forces them, then recorded in spec 18 (`11-pages-and-dolt-snapshots.md` part 4). **Large values C** (#181): a throwaway prototype measures the gates first; a clean format break is acceptable until step 8 (`14-large-values-and-toast.md` C7). **C suite on PRs** (#234): settings applied by Claude with `gh api` after asking; no merge queue, no up-to-date rule, a tracking issue on `master` failures; only the C suite gate required at first (`16-correctness-on-prs.md` part 7). **S suite on PRs** (#240): no dedicated host, so hosted runners only and timing reported, not gated (`17-performance-on-prs.md` part 7). #155 and #180 closed as done |
| Default writer (#148) | **Open:** whether `dolt.writer` defaults to the session or the role; revisited with data from the `owned` mode's use (`11-pages-and-dolt-snapshots.md` 6.2) |

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
   **Review (2026-10-14):** the starting targets aim at an item count
   (about 31 leaf items per node, assuming 256-byte items), so a node of
   narrow rows can hold about 1 KiB in an 8 KiB page; Dolt and DoltLite
   split by accumulated bytes (a Weibull rule around 4 KiB, none below
   512 bytes, forced at 16 KiB). The forced split at a full page also
   makes boundaries depend on `BLCKSZ` (question 45). Measure occupancy
   first; the rule and targets are settled with the format freeze (4g).
   **Measured (2026-10-14, 4e.1 node counters):** about 31 items per leaf
   holds 800 bytes of row-identity entries and 3 KB of narrow rows per
   8 KiB page, so a 20,000-row scan reads 1,334 nodes where nodes filled
   to about 4 KB would make it about 600. **Plan (user, 2026-10-14):**
   settled in step 5, which may still change the 4g freeze (the format is
   provisional until every engine has been evaluated): a size-weighted
   key-only chunker (`keyhash_weibull`, as Dolt's default) measured
   against `keyhash`; what is measured is listed in `07-roadmap.md`,
   step 5.
   **Direction (user, 2026-10-14):** Dolt's rules decide what a chunk is,
   Postgres's pages where it lives. Tree boundaries by `keyhash_weibull`
   (key-only hash, Weibull on canonical bytes, minimum and maximum fixed by
   the format); value bytes by a rolling hash (question 48); chunks packed
   into write-once pages (question 45). Locks and SSI predicates are keyed
   on rows and key ranges, never nodes (`02-postgres-integration.md`, A4),
   so boundaries may move with value sizes without disturbing them, which
   removes the reason for key-only, item-count boundaries.
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
    **Decided 2026-10-14 (user):** refused, with a clear error: a prolly
    index lives in its table's root set and points at row keys, which heap
    tables don't have. Can be revisited.
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
    **Decided (user, 2026-10-14):** the proposal: checkout resets the
    backend's caches.
24. **Code that reads catalog tuples' xmin/ctid.** Some code reads system
    columns of catalog tuples (for example to detect changed functions or
    for in-place updates). Those must work with synthetic TIDs and
    emulated xmin, or change. In-place updates themselves: proposed as
    unversioned side records (`09-versioned-catalogs.md` part 2).
25. **Heap user tables in a versioned database.** A heap table's definition
    would change with the branch while its data doesn't. Forbid heap tables
    in a versioned database, or treat their catalog rows as unversioned?
    **Decided (user, 2026-10-14):** a permanent heap table in a versioned
    database is refused. Temporary and unlogged tables, which use heap
    (question 28), are left out of commits: the canonicalizer skips their
    catalog rows.
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
    **Decided (user, 2026-10-14):** shared catalogs are prolly tables, but
    unbranched and outside every commit (Dolt doesn't version users either).
    This narrows question 21's decision: per-database catalogs are
    versioned; cluster-wide ones are stored in prolly trees without history.
28. **Temporary and unlogged prolly tables.** The cluster-wide store is
    WAL-logged and in shared buffers. Temporary tables use backend-local
    buffers, and unlogged tables are reset after a crash. Do they get their
    own stores, stay heap, or aren't they supported?
    **Decided (user, 2026-10-14):** they use heap. They are never part of a
    commit (Dolt's temporary tables aren't either), so heap keeps their
    Postgres behaviour exactly; a temporary or unlogged table created while
    prolly is the default access method gets heap, and an explicit
    `USING prolly` on one is refused.
29. **Tablespaces.** With one store, `CREATE TABLE … TABLESPACE` can't place a
    prolly table's data. One store per tablespace, or tablespaces ignored for
    prolly tables?
    **Decided (user, 2026-10-14):** the clause is accepted and ignored for
    prolly tables, with a NOTICE; one store per tablespace later, if needed.
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
    **Changed 2026-10-14 (user), for 4d:** the local root keys relations by
    **relfilenumber**, not OID. TRUNCATE, ALTER TABLE rewrites, CLUSTER,
    VACUUM FULL and REINDEX give a relation new storage (and rewrites swap
    `relfilenode` between OIDs in `swap_relation_files`); keyed by storage
    number, each new storage is a new tree, the old one stays reachable
    until commit, so transactional truncate and rollback of a rewrite
    behave as on heap and SET TABLESPACE is a pointer copy. And the local
    root has a **third sub-root, local**, holding per-relation metadata that
    is never canonical (TID maps, question 37); the canonical root keeps
    the two-sub-root shape.
34. **Keyless catalogs.** `pg_depend` and `pg_shdepend` have no unique key.
    Key their trees by row content hash with a count (Dolt's keyless tables),
    or by a synthetic row id?
    **Decided (user, 2026-10-14):** by row content hash with a count, as
    keyless user tables are.
35. **Canonicalizer details.**
    - Expressions are stored as deparsed SQL, but `ruleutils.c` output can
      change between Postgres versions, which would change hashes on upgrade.
      Pin a canonical deparse format, or version it with the format stamp?
      **Decided (user, 2026-10-14):** a pinned canonical deparse, under the
      format version stamp: a deparser change needs a format migration,
      never a silent hash change.
    - User data that holds OIDs (`oid` and `reg*` columns, large object
      references) is local by nature. Translate where the type says what it
      refers to (`regclass` → name), and refuse or pass through the rest?
      **Recommended (2026-10-14):** this is a hole in the "no local OIDs"
      promise today (the canonical encoder's fallback uses a type's send
      function, and `regclasssend` sends the OID). Portable codecs for the
      `reg*` types (by qualified name, as their output functions print
      them); a version-control commit refuses a column of a type with no
      known OID-free encoding (`oid` itself, unknown extension types), with
      a clear error, rather than hashing a local identifier.
      **Decided (user, 2026-10-14):** as recommended.
      **Built (#217, 2026-10-17), the refusal:** a commit refuses a column
      whose type holds OIDs: `oid`, `oid8` and the `reg*` types (refused
      until their portable codecs are built), `pg_node_tree`, and the
      domains (contrib's `lo`), arrays, composites and ranges made of
      them; status and diff
      list each such column as not covered (`14-large-values-and-toast.md`
      part 7).
    - Types with no binary send format (`aclitem`, user and extension
      types declared with text I/O only). **Decided (user, 2026-10-15):**
      stored as their text output, with the settings that affect output
      fixed while encoding, and the column's canonical schema recording
      the text form.
    - Column identity across renames: Dolt keeps column tags across
      renames, so a rename on one branch and an edit on another merge;
      identity here is by name. Rows are encoded by column position, so a
      rename leaves data trees untouched; the merge rules for a column
      renamed on one branch and changed on the other are still to decide.
      **Decided (user, 2026-10-14):** a rename merges as a drop and an add,
      so a column renamed on one branch and changed on the other is a
      schema conflict. Stable column tags, as Dolt's, are a later option.
    - Rename detection in diff: by matching content hashes, with what
      threshold for "same table, changed"?
36. **Keying user tables.** Postgres tables need not have a primary key,
    the table AM doesn't see the key at insert, and history independence
    rules out insertion-order row ids.
    **Decided 2026-10-14 (user):** Dolt's model. A table with a primary key
    is keyed by its encoded key columns; a table without one is keyed by
    the row's content hash, with a count of identical rows as part of the
    value. ADD or DROP PRIMARY KEY rebuilds the table's tree. Rejected:
    always keying by content with the primary key as a separate index tree
    (diff and cell-level merge would need a join), and synthetic row ids
    (history-dependent).
37. **Where the TID map lives, and where TIDs come from.** Non-prolly index
    AMs store TIDs persistently, so the synthetic TID → row map must be
    resolvable after restart and per branch.
    **Decided 2026-10-14 (user):** one tree per relation, in the local
    sub-root of the working-set root, versioned with the working set,
    WAL-logged by the node store, never part of a canonical root. Rejected:
    one unversioned cluster-wide map (checkout could leave index entries
    pointing at TIDs the branch doesn't have).
    **Changed 2026-10-14 (user), TID source:** TIDs come from a per-table
    counter, not from a hash of the row. The counter is block 0 of the
    table's main fork, reserved by each backend 1024 numbers at a time with
    generic WAL (as sequences cache values); aborts and crashes skip
    numbers, never reuse them. TID number n is item pointer
    (n / MaxHeapTuplesPerPage, n % MaxHeapTuplesPerPage + 1), so TIDs are
    in insertion order. The map holds TID → (content hash, ordinal, xmin,
    key), or for a gone version (cid, xmin, successor, its row, kept like a
    dead tuple until VACUUM), and (key, ordinal) → TID; scans find TIDs
    through the second. Why: TIDs must never be given to a second version
    while an index can hold them, and index entries outlive aborts (a row
    inserted again after a rollback got the same content-hashed TID, so
    indexes held two entries for it and returned it twice); and an index
    serves every branch of its table, so a TID must not name different
    versions on different branches, which per-branch collision probing
    can't see. One counter per table storage serves every branch.
    Rejected: hashed TIDs salted with the xid (still collide across
    branches), and hashed TIDs plus a log of issued TIDs (the log would
    have to be a cluster-wide allocator anyway). Consequences: TID reuse
    after VACUUM is a requirement of step 4f (question 40), several writing
    nodes would need per-node ranges, and clones can avoid building the map
    (question 41).
38. **Row locks for a single writer.** Until the concurrency design (step
    4e), a prolly table has one writer at a time. Row locks (SELECT FOR
    UPDATE/SHARE, and the FOR KEY SHARE locks of foreign-key checks) need
    a rule that fits it.
    **Decided 2026-10-14 (user):** a per-table writer lock, separate from
    the relation lock (so it doesn't conflict with the RowExclusiveLock
    every writing statement already holds, which made concurrent writers
    deadlock in 4d.1). Writes, FOR UPDATE and FOR NO KEY UPDATE take it
    exclusively; FOR SHARE and FOR KEY SHARE take it in share mode, so
    many sessions can hold share locks while writers wait. Coarser than
    heap (per table, not per row), with the same kinds of locks
    conflicting. Rejected: every row lock exclusive (every foreign-key
    check would serialize all inserts into child tables).
39. **Finding a row's new version.** In READ COMMITTED, an UPDATE, DELETE
    or row lock that finds its row changed by a transaction that committed
    while it waited follows the change to the new version (heap follows
    t_ctid).
    **Decided 2026-10-14 (user):** forwarding entries in the TID map: an
    update records old TID -> new TID, for primary-key and keyless tables
    alike; currtid and WHERE CURRENT OF use them too. They stay until
    VACUUM forgets the old TID. Rejected: following by primary key (a
    keyless table's updated row would be treated as deleted, unlike heap).
40. **TID reuse.** A table has about 1.25×10¹² counter TIDs; every insert
    and update uses one. At 100k updates a second that lasts about five
    months; table rewrites (TRUNCATE, VACUUM FULL, CLUSTER, ALTER TYPE)
    start a new counter. **Requirement for step 4f:** VACUUM, once it has
    removed every index entry for TIDs that no retained root's map names,
    lets the counter hand those numbers out again (heap does the same with
    line pointers), across every branch and retained root.
41. **Clone without building TID maps.** TID maps are local and never
    shipped, so a clone has to give the received rows TIDs. Plan for V4:
    at clone, reserve a counter range [base, base + N) per table and let
    TID base + i mean the row at position i of the cloned tree (prolly
    trees keep subtree counts, so position ↔ row is O(log N) both ways);
    rows written later get explicit map entries above the range, and a
    scan finds an unchanged row's TID by walking the cloned root beside the
    current one (O(changes)). Then a clone costs O(chunks), as Dolt's does,
    for tables whose indexes are prolly indexes; non-prolly indexes on the
    receiving cluster are rebuilt (O(N log N) each), as in a restore.
    Content-derived TIDs couldn't avoid an O(N) map: a hash can't be
    inverted. Not built until clone is.
42. **What a prolly index entry points at.**
    **Decided 2026-10-14 (user):** the row's key in its table (and its
    ordinal among identical rows), never a TID. An entry is the encoded
    index columns, then the row key, then the ordinal, with no value; the
    tree records its own description (the index columns' types, operator
    families, collations and options) once, as a table's tree records its
    keying, and is found in the root under the index's own relfilenumber
    (by name in canonical roots). So the index tree is a function of the
    table's rows: versioned with the branch, merged and shipped with clones
    like the data. Scans map row key -> TID through the TID map. A change
    of the table's keying (which changes every row key) rebuilds its
    prolly indexes with the data tree. As in Dolt, where a secondary index
    row is the index columns then the primary key columns. Rejected:
    entries pointing at TIDs (local, so the index couldn't be part of a
    commit or ship with a clone).
43. **Removing a prolly index's entries.** Postgres calls an index AM only
    to insert; heap leaves old entries for VACUUM.
    **Decided 2026-10-14 (user):** the table AM removes the old version's
    entries in the same write as the delete or update, computing index
    expressions and partial-index predicates itself. The index always
    holds exactly the table's rows, so commits stay history-independent
    and index-only scans can be exact. Rejected: leaving stale entries for
    VACUUM (the index tree would depend on when VACUUM ran).
44. **ANALYZE and sampling of prolly tables.** Postgres samples a table by
    physical block of its main fork; a prolly table's main fork has one
    block (the TID counter), and table AMs have no hook for their own
    sampler.
    **Decided 2026-10-14 (user):** an optional table AM callback in
    `analyze.c` lets an AM supply its row sampler, as FDWs already can;
    the tree and engine find the key of the n-th row from subtree counts,
    so ANALYZE reads only the sampled rows (O(sample × log N)) and
    TABLESAMPLE SYSTEM picks groups of rows in key order the same way.
    Tables of other AMs are untouched. Rejected: reading every row and
    letting reservoir sampling pick (O(N) for every ANALYZE, autovacuum's
    included).
    **Changed 2026-10-14 (user), after review:** keyless tables are read
    whole, into a reservoir over rows: an item holds identical rows, so
    picking items gives every row the same chance but with huge variance
    (a value that is most of the table, in one item, is usually missed).
    Rejected: subtree counts weighted by rows (a format change through the
    tree, engine and keying interfaces). A table the transaction has
    written is read whole too (positions cover only the committed tree).
45. **Canonical chunks and `BLCKSZ`.** Boundaries depend on page capacity
    (a forced split when the page is full), so identical content on builds
    with different page sizes makes different trees and different hashes;
    the canonical form must not depend on a build option.
    **Recommended (2026-10-14):** the canonical chunk rule has its own size
    limits, fixed by the format and independent of `BLCKSZ` (byte targets
    from question 3, an upper bound that fits an 8 KiB page); an engine
    whose page can't hold a canonical chunk stores it in more than one
    block. Alternative: require `BLCKSZ` = 8192 for prolly databases. To
    settle with the format freeze (4g).
    **Direction (user, 2026-10-14), settled in step 5:** the chunk rule's
    limits are format constants, the maximum about 7.5 KB so a chunk fits
    an 8 KB page after its overhead; prolly tables require `BLCKSZ` of at
    least 8192, so a canonical chunk always fits one block and no engine
    needs chunks spanning blocks. Chunks are packed into pages by a
    packing node store (several chunks per page, written once and sealed;
    references are block, offset and length; compaction moves chunks and
    updates the hash -> location map), measured against one chunk per page
    (`07-roadmap.md`, step 5).
46. **Collation versions in canonical form.** Keys ordered by a collation
    (the key encoder's comparator fallback) are in an order that can change
    with the collation library's version, so two clusters can order the
    same keys differently.
    **Recommended (2026-10-14):** the canonical schema records each key
    column's collation and its version; transfer and merge between
    mismatched versions refuse, or rebuild the affected trees. To settle
    with the format freeze (4g).
    **Decided (user, 2026-10-14):** recorded per key column; merge and
    transfer between mismatched versions refuse.
47. **Bounded transaction memory.** D1 keeps a transaction's writes, with
    their command and subtransaction versions, in memory until commit, with
    no limit; Dolt flushes after about 64K pending entries, DoltLite at
    about 64 MiB or a million entries.
    **Recommended (2026-10-14), for 4e:** past a threshold, versions older
    than the current command are flushed into an intermediate tree, keeping
    a root per open savepoint (Dolt's checkpoints) so rollback still works;
    commit replays edits recovered by diffing the intermediate tree against
    the base, which is why subtree-skipping diff comes with it.
    PREPARE TRANSACTION after prolly writes uses the same flush: the
    prepared transaction's write set becomes an intermediate tree whose
    root is kept in the two-phase state file (user, 2026-10-14, 4e.4).
    **Decided (user, 2026-10-14):** the threshold is a setting,
    `prolly.write_set_memory`, 64 MB by default.
    **Built (4e.4):** named `prolly_write_set_memory`, as core settings
    have no dots. The intermediate tree is a delta keyed as the write set
    (each key's newest version, and the one the current command sees),
    read in key order at commit, rather than a tree diffed against the
    base; PREPARE records whole trees and COMMIT PREPARED replays them by
    diff.
48. **Large values.** Items must fit a node, and prolly tables have no
    TOAST table.
    **Recommended (2026-10-14), for 4f:** out-of-line value trees in the
    same root, as Dolt's blobs, reusing Postgres's compression methods; the
    node format records where a value refers to another tree, so walk,
    reclamation and transfer find every reference. Without that contract a
    value tree would be invisible to GC and to push. A value's bytes are
    split by a rolling hash (buzhash) over the bytes, as Dolt's blobs are:
    there are no keys to hash (user, 2026-10-14; the chunk rule is settled
    in step 5).
    **Decided (user, 2026-10-14):** a value goes out of line past a size
    fixed by the format, about 2 KB (near TOAST's threshold). Its canonical
    bytes are uncompressed, so a hash never depends on a compression
    method's version; compressing pages is a later storage choice.
    **Built (4f.1):** in the tree layer, for every tree; chunks keyed by
    offset for now.
    **Changed (#218–#221, 2026-10-17):** value trees are addressed by byte
    counts; a column whose canonical bytes are longer than 2 KB is stored
    as a reference to a value tree of its own, the whole row out of line
    only when still long; leaves record the references their values hold,
    so walks find them; a memo from TOAST pointers to references spares a
    fold reading unchanged values; apply keeps a row's own TOAST pointer
    for a value that didn't change (`14-large-values-and-toast.md` part 5).
49. **What reclamation must keep.** Reachability from branch heads alone
    isn't enough.
    **Recommended (2026-10-14), for 4f:** the live roots are every
    branch's working-set head, every commit's canonical and local roots,
    every root a running transaction or snapshot holds (its start, its
    savepoints, intermediate trees of its write sets), roots pending in the
    commit log, and the value trees all of these refer to. Gone row
    versions still named by a live TID map are a separate retirement,
    under Postgres's visibility and index-lifetime rules (question 40).
    **Decided (user, 2026-10-14):** reclamation of nodes is an explicit
    `prolly_gc()`, as `dolt gc` is: finding what is reachable walks every
    live root of the cluster's store. VACUUM retires a table's own gone
    versions and TIDs. Automatic reclamation is later.
    **Built (4f.2):** `prolly_gc()`, mark and sweep from every live root
    the commit pipeline holds, freed blocks reused once older snapshots
    end; writers and it are kept apart by a store lock. **(D1)** All of
    this went with D1 (#183).
    **For the page design (step 7, decided 2026-10-17, built by
    2026-10-18):** a copying collector of the chunk store
    (`reclaim_copying.c`) from the refs' roots (`11-pages-and-dolt-snapshots.md`
    8.1), run by `dolt_gc()` and started by autovacuum, with transactions'
    in-memory chunks kept by a horizon and every appended batch's
    references checked; spec 14's C will add the versioned heaps' hash
    pointers and replication slots' chunks as roots.
50. **Row lock identity (K1).** Postgres's lock manager names a lock by a
    fixed-size tag, so a row key that doesn't fit must be reduced.
    **Decided (user, 2026-10-14):** the tag is (database, relation, 64-bit
    hash of the row key). A collision makes a writer wait needlessly, never
    lets two writers through; nothing is stored in the tree, and PREPARE
    carries the locks over as it does any lock.
51. **Serializing commits to a branch.** **Decided (user, 2026-10-14):**
    one commit lock per branch. A transaction builds its new trees before
    taking it; only the replay onto a newer head runs under it. Group
    commit waits until step 5 shows the lock matters.
52. **SQL interface for version control.** **Decided (user, 2026-10-14):**
    Doltgres's: functions `dolt_commit()`, `dolt_checkout()`,
    `dolt_branch()`, `dolt_merge()`, `dolt_reset()`, and system tables such
    as `dolt_log`, `dolt_status`, `dolt_branches` and `dolt_diff_<table>`.
53. **How a session picks a branch.** **Decided (user, 2026-10-14):** both
    of Doltgres's ways: `dbname/branch` as the connection's database name,
    and `dolt_checkout()` within a session.
54. **Which databases are versioned.** **Decided (user, 2026-10-14):**
    opt-in per database (`CREATE DATABASE … WITH (versioned)`, or a
    template), so Postgres's own tests and the heap baseline run on plain
    databases.
55. **Merge conflicts.** **Decided (user, 2026-10-14):** Dolt's model. Row
    conflicts are recorded in `dolt_conflicts_<table>` and block commit
    until resolved; schema conflicts abort the merge with an error naming
    the objects.
56. **Commit author.** **Decided (user, 2026-10-14):** a setting,
    `dolt.author`, defaulting to the session's user, overridable per
    commit.
