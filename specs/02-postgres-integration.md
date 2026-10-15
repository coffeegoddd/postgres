# 02. Integrating with Postgres

Every engine in `04-candidate-designs.md` must satisfy these interfaces. They
are written in terms of the coordination-in-the-tree candidate (option 8 in
`01-context.md`), but most apply to all candidates. Line numbers were checked
against this checkout when written; re-verify before relying on them.

The tree meets Postgres at three levels:

- **A.** Pluggable APIs that already exist and suffice.
- **B.** Places with no hook, where the fork must patch core.
- **C.** The tree's own internal API, which A and B call.

## A. Existing pluggable APIs

### A1. Table AM (`TableAmRoutine`, `src/include/access/tableam.h`)

| Callbacks | What the tree must provide |
|---|---|
| `slot_callbacks`, `scan_begin/getnextslot/rescan/end` | Cursor over (snapshot root ⊕ own mutation map); a slot that decodes lazily from node bytes |
| `parallelscan_*` | Range splitting by leaf ordinal |
| `index_scan_*` (which install `xs_getnext_slot`), `tuple_fetch_row_version`, `tuple_tid_valid`, `tuple_satisfies_snapshot`, `tuple_get_latest_tid` | TID ↔ key resolution (B7). On this master the table AM decides index-only visibility itself (the visibility map is only consulted inside heap's index-scan code), so exact index-only scans need no core patch |
| `tuple_insert`, `multi_insert` | Writes go to the private mutation map, tagged with the command id (Halloween protection, `TM_SelfModified`) |
| `tuple_insert_speculative` / `tuple_complete_speculative` | Insert intent carrying the speculative token, so `ON CONFLICT` waiters use `SpeculativeInsertionWait` |
| `tuple_update`, `tuple_delete`, `tuple_lock` → `TM_Result` + `TM_FailureData{ctid, xmax, cmax, traversed}` | Lock entries hold xid or multixact, cid and mode. `TUPLE_LOCK_FLAG_FIND_LAST_VERSION` re-reads from the latest head |
| Lock modes KEY SHARE / SHARE / NO KEY UPDATE / UPDATE | The full conflict matrix, including detecting whether an update changes key columns |
| Wait policies Block / Skip / Error | `XactLockTableWait`, `ConditionalXactLockTableWait`, or error |
| `relation_set_new_filelocator` | New empty root; return invalid freeze xid / multixact (no xids in content, no wraparound vacuum) |
| `relation_nontransactional_truncate`, `relation_copy_data`, `relation_copy_for_cluster` | Root reset or copy, O(1) for content-addressed data |
| `relation_vacuum` | Engine-specific: reclaim roots/nodes (D1–D3), or prune in-page versions (D4) |
| ANALYZE / TABLESAMPLE / size estimate callbacks | Leaf ordinals as pseudo-blocks; row counts from subtree counts |
| `index_build_range_scan`, `index_validate_scan` | Full snapshot scan to feed index builds |
| `relation_needs_toast_table` → false | Large values stored out of line by the tree (blob subtrees) |

### A2. Index AM (`IndexAmRoutine`, `src/include/access/amapi.h`)

A prolly-native index is a second tree per index, from the **same root set** as
its table. Built in 4d.3: an entry is the index columns, then the row's key in
its table and its ordinal among identical rows (question 42), so the tree depends
only on the table's rows; the table AM removes a row version's entries in the
same write (question 43), since Postgres calls an index AM only to insert.
Prolly indexes exist only on prolly tables (question 17). Not built yet: bitmap
scans, INCLUDE columns, parallel scans and builds, predicate locks.

- `ambuild`: bulk build from a sorted stream via the chunker.
- `aminsert` with `UNIQUE_CHECK_YES` (insert intent + wait), and
  `UNIQUE_CHECK_PARTIAL` / `UNIQUE_CHECK_EXISTING` for deferred unique
  constraints (recheck at commit).
- Scans, mark/restore, parallel scans by ordinal range.
- `amcanreturn` true: index-only scans are exact.
- Key encoding must be memcmp-comparable **and** agree with every btree
  opclass and collation (B6).

### A3. WAL resource managers (`RmgrData`, `src/include/access/rmgrlist.h`)

The storage is in core, so its resource managers are built in (listed in
`rmgrlist.h`) rather than registered with `RegisterCustomRmgr`. Built in step
4b: `Prolly` for records from the log interface and `ProllyStore` for the
node store's own structures, with redo dispatched to the implementation named
in each record (`src/include/access/prolly_xlog.h`).

- `rm_redo`: replay the engine's WAL records (logical mutations, deltas or page
  images depending on the engine), plus root installs (reset, fast-forward,
  checkout).
- `rm_desc`, `rm_identify`: `pg_waldump` (generic for now; open question 16).
- `rm_startup`, `rm_cleanup`: rebuild redo-time branch heads.
- `rm_mask`: only if nodes are buffer-manager pages.
- `rm_decode`: logical decoding from row-level records, giving pgoutput and
  logical replication natively. Engines whose WAL is not row-level (D1, D3,
  D4) must also emit row-level information for decoding when
  `wal_level=logical`. Deferred past the first engine (open question 31).

### A4. Locking, waiting, SSI

- Row waits via `XactLockTableWait` / `ConditionalXactLockTableWait`
  (`src/backend/storage/lmgr/lmgr.c`). The coordination state only records
  *which* xid to wait on, so deadlock detection is unchanged.
- Multiple lockers via multixacts, as heap's `xmax` does.
- SSI (`src/backend/storage/lmgr/predicate.c`): map key ranges onto
  predicate.c's vocabulary. Locks are never keyed on tree nodes or pages
  (user, 2026-10-14): node boundaries can move when a value changes size,
  and locks that followed them would conflict falsely or miss conflicts.
  Key ranges get stable synthetic block numbers from a partition of the key
  space that depends on keys alone (a boundary where a key's hash falls
  below a fixed threshold, whatever the values) → `PredicateLockPage`; keys
  → `PredicateLockTID`; a range boundary key inserted or deleted →
  `PredicateLockPageSplit/Combine`; writes → `CheckForSerializableConflictIn`;
  reads that skip an invisible newer version →
  `CheckForSerializableConflictOut`. Row locks are keyed on rows the same
  way.

### A5. Shared memory and lifecycle

- `shmem_request_hook` / `shmem_startup_hook`, LWLock tranches, DSA / dshash,
  condition variables.
- `RegisterXactCallback` / `RegisterSubXactCallback`: drop a transaction's
  locks and mutation map on abort; on subtransaction abort drop only entries
  tagged with that subxid.
- ResourceOwner: pins on roots and nodes are released on error.

## B. Core patches the fork owns

| # | Gap | Patch point | Why |
|---|---|---|---|
| B1 | Atomic commit publish | `CommitTransaction` between `RecordTransactionCommit` and `ProcArrayEndTransaction` (`src/backend/access/transam/xact.c`), and the prepared path | Branch roots must change at the instant the xid becomes visible |
| B2 | Snapshots carry a root epoch | `SnapshotData`, `GetSnapshotData` (`procarray.c`), snapmgr copy/serialize, exported and parallel snapshots | A snapshot must map to exactly one root set, consistent with heap tables in the same transaction |
| B3 | Standby visibility | `xact_redo_commit` | On a hot standby, roots become visible exactly when the commit record replays |
| B4 | Checkpoint and node I/O | Was: an `smgrsw[]` entry or a `CheckPointGuts` hook. **Decided 2026-10-13: neither.** Nodes are pages of a cluster-wide node store at fixed relation file numbers in the global tablespace, read through the existing smgr (md) and buffer manager, so checkpoints, checksums and full-page writes need no patch (open question 1; built in step 4b) | No checkpoint hook exists for extensions; being in core with ordinary relation files avoids needing one |
| B5 | 2PC | New `TWOPHASE_RM_*` id (fixed list in `twophase_rmgr.h`) | PREPARE persists the mutation map and locks; recovery re-acquires locks |
| B6 | Key encoding | New opclass → encoder registry | memcmp keys for every btree opclass and collation, with a comparator fallback |
| B7 | Row identity | None (synthetic TID + map in the tree), or variable-width row ids in `ItemPointer` users | TIDs flow through index scans, triggers, EPQ, `CURRENT OF`, `ctid` |
| B8 | Parallel workers see the leader's writes | `InitializeParallelDSM` / `EnterParallelMode` | The private mutation map must be in DSA, or flushed to a private root, before workers start |
| B9 | Branches | `namespace.c` (schema lookup and search path), `InitPostgres` | `public@branch` schema names, `dbname=db/branch` connection strings (one option; not decided) |
| B10 | Versioned catalogs | `catalog/indexing.c` (`CatalogTuple*` through the table AM), in-place updates (`genam.c`), `heap_getnext` catalog loops, bootstrap (`bootparse.y`, `bootstrap.c`), `relcache.c` (`formrdesc`, nailed relations, init files), relation mapper, cache reset on checkout | Every catalog is a prolly table (decided 2026-10-13). Inventory and proposals in `09-versioned-catalogs.md` |
| B11 | Client encoding override | `src/backend/utils/mb/mbutils.c` (`SetClientEncodingOverride`) | Built 2026-10-13. Binary send/receive of text-like types converts through the client encoding; canonical values must not depend on a session setting, so prolly storage runs them with the encoding fixed to UTF-8 |

## C. Sketch of the tree's internal API

All engines implement this so the A/B adapters, the tests and the benchmark
harness are shared. Names are provisional.

```c
/* roots & snapshots */
PtRootSet  *pt_snapshot_roots(Snapshot snap);
PtCursor   *pt_seek(PtRootSet *rs, PtTxn *txn, PtTree t, PtKey k, CommandId cid);
bool        pt_next(PtCursor *c, PtKey *k, PtValue *v);
uint64      pt_count_range(PtRoot r, PtKey lo, PtKey hi);

/* private writes */
void        pt_put(PtTxn *txn, PtTree t, PtKey k, PtValue v, CommandId cid);
void        pt_delete(PtTxn *txn, PtTree t, PtKey k, CommandId cid);
void        pt_subxact_rollback(PtTxn *txn, SubTransactionId sub);

/* coordination (implemented by K1/K2/K3, see 04) */
PtLockResult pt_lock(PtTxn *txn, PtTree t, PtKey k, LockTupleMode m,
                     LockWaitPolicy w, PtLockFailure *out);
PtLockResult pt_intent_insert(PtTxn *txn, PtTree t, PtKey k, uint32 spec_token,
                              PtLockFailure *out);
void         pt_range_lock(PtTxn *txn, PtTree t, PtKey lo, PtKey hi);
void         pt_release_all(PtTxn *txn);

/* commit path */
void        pt_precommit(PtTxn *txn);
void        pt_publish(PtTxn *txn);
XLogRecPtr  pt_log(PtTxn *txn);
void        pt_prepare(PtTxn *txn);

/* durability & housekeeping */
void        pt_checkpoint(void);
void        pt_redo(XLogReaderState *rec);
void        pt_install_roots(PtBranch b, PtRootSet *rs);
void        pt_gc_roots(void (*cb)(PtHash, void *), void *arg);

/* canonical form (required of every engine, see 05) */
PtHash      pt_canonical_root(PtRootSet *rs, PtTree t);
void        pt_export_chunks(PtRootSet *rs, PtTree t, PtChunkSink *sink);
```

## Invariants

1. Hashes cover canonical content only. Nothing transactional (locks, xids,
   markers, physical locations) and nothing local to a cluster (OIDs,
   statistics) is ever part of a hashed chunk.
2. A snapshot sees exactly one root set (or, in D4, one visibility cut), and it
   agrees with procarray visibility for every xid.
3. A lock entry never outlives its transaction or subtransaction, except for
   prepared transactions and the commit markers below.
4. Commit publish never fails on conflicts: anything that could conflict was
   locked earlier, so the rebase at commit is mechanical.
5. Node rewrites (splits, merges, boundary shifts) preserve lock entries by
   key and notify SSI.
6. Table and index trees always come from the same root set.
7. **All engines produce identical canonical chunks and root hashes for the
   same logical data** (see `05-storage-targets.md`).
8. **History independence:** a version-control commit's root hash depends only on
   the logical content (schema and data) it holds, never on the operations
   that produced it, on the cluster, or on the platform
   (`09-versioned-catalogs.md`).

## Three design changes found necessary for option 8

These came out of checking option 8 against the interfaces above. They apply
to any copy-on-write engine (D1–D3) that uses K2.

1. **Ordered commit pipeline.** Rebasing under `ProcArrayLock` would stall every
   snapshot cluster-wide. Instead:
   1. rebase under a per-branch commit lock, write the WAL record, enqueue the
      new root in branch commit order, release the lock **before** fsync;
   2. flush with normal group commit;
   3. publish roots in queue order in `ProcArrayEndTransaction` (an O(1) pointer
      swap once every predecessor is visible).

   Abort after rebase is impossible short of PANIC, so the queue has no gaps.
   This is the most performance-critical code in the system.
2. **Commit markers kept until the xmin horizon.** REPEATABLE READ's "could not
   serialize access due to concurrent update" and SSI's conflict-out checks
   need to know a key was written by a transaction that committed after the
   reader's snapshot. On commit, lock entries become
   `COMMITTED_WRITE(key, xid)` markers, kept (unhashed, unpersisted) until the
   xmin horizon passes them.
3. **Per-version TIDs from a per-table counter.** Non-prolly index AMs (GiST,
   GIN, BRIN, pgvector, bloom) assume one TID names one row version, and keep
   entries for TIDs whose transaction aborted. So TIDs come from a per-table
   counter that aborts don't roll back (like heap's line pointers), shared by
   every branch of the table, with a map TID → (key, content hash, xmin) and
   (key, ordinal) → TID (question 37, changed 2026-10-14). Content-derived
   TIDs were tried first and dropped: a row inserted again after an abort got
   the TID its aborted insert left in every index, and hash collisions across
   branches can't be seen by any one branch's map. Dead TIDs for
   `ambulkdelete` = diff(root at last vacuum, current root). Long-term fix:
   wide row identifiers (B7).

## What can only be emulated

| Feature | Treatment |
|---|---|
| `xmin` / `xmax` / `cmin` / `cmax` columns | Synthetic (open question 24 for catalog rows). A content-derived `xmin` that changes only when the row changes keeps ORMs that use it as an optimistic-concurrency token working; `age(xmin)` is meaningless. D4 may be able to expose real values |
| `ctid` as a physical location | Synthetic |
| `pageinspect`, `pg_visibility`, heap amcheck, `pg_surgery`, `pg_repack` | Need prolly equivalents |
