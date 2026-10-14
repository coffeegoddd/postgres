# 07. Roadmap

The roadmap is these six steps and ends at step 6. Nothing is planned beyond
it; what comes next is decided in step 6's discussion. Do the steps in order,
and don't start a step until a human asks for it.

## 1. Stub the interfaces for every engine

Make every planned interface exist and fail cleanly, before any of it works.

- Register table AMs for engines D1–D4 (`04-candidate-designs.md`) in the
  catalog (`pg_am`, handler functions in `pg_proc`): `prolly_d1`,
  `prolly_d2`, `prolly_d3a`, `prolly_d3b`, `prolly_d4`. D5 is not stubbed: it
  waits on the storage-target question.
- Each `TableAmRoutine` callback, and the prolly index AM's `IndexAmRoutine`
  callbacks, raise
  `ereport(ERROR, errcode(ERRCODE_FEATURE_NOT_SUPPORTED), …)` with a message
  naming the engine and the callback.
- Define **every interface** in `00-architecture-principles.md` as a header
  with its routine struct and written contract, a registry for selecting
  implementations by name, and a stub implementation of each. This includes the
  engine (D1–D4) and coordination (K1–K3) interfaces, so engines and modules can
  be paired as in `04-candidate-designs.md`.
- A conformance-test skeleton per interface that runs against every registered
  implementation (the stubs should fail it with "not implemented").
- Stub SQL functions for the version-control operations the V suite measures
  (branch create, checkout, commit, diff, merge, clone/push/fetch).
- Code lives under `src/backend/access/prolly/` and
  `src/include/access/prolly*.h`.

**Exit criteria:** the tree builds with no new warnings; `make check` passes
unchanged for heap; each stub raises its "not implemented" error when called
through SQL; every interface in `00-architecture-principles.md` has a header,
a stub and a conformance-test skeleton; the Postgres-facing adapters call only
interfaces.

**Status (2026-10-13): built, not yet reviewed.**

| Exit criterion | Result |
|---|---|
| Builds with no new warnings | Yes (autoconf build, gcc, `--enable-cassert`). The meson build files were written but not tried: meson isn't installed locally |
| `make check` passes unchanged for heap | 239/239. Four expected outputs gained rows for the new AMs and opclasses (`amutils`, `create_am`, `opr_sanity`, `psql`); no heap behaviour changed |
| Each stub raises "not implemented" through SQL | Yes for creating a table with each prolly AM (reaches the engine's `tree_create`), `SET ACCESS METHOD`, `CREATE INDEX … USING prolly`, and every version-control function. Table AM callbacks other than the storage ones can't be reached from SQL until a table can be created |
| Every C interface has a header, a stub and a conformance skeleton | Yes: 16 interfaces (the 15 planned plus diff/merge), `src/test/modules/test_prolly`. The suite also runs two test-only hash implementations to show it passes and fails correctly |
| Adapters call only interfaces | Yes: `prolly_tableam.c`, `prolly_indexam.c` and `prolly_vcs.c` go through `prolly_config.h` and `PROLLY_CALL` |

The benchmark workload, metric collector and result sink interfaces are part
of step 2.

## 2. Build the benchmark harnesses

Two separate suites, because Postgres's existing engines can't do version
control (`06-benchmarking.md`):

- **S suite (storage):** workloads S1–S9 that run on any table AM, so heap (B0)
  and every prolly engine are measured the same way.
- **V suite (version control):** workloads V1–V4 for prolly engines only.
  No heap baseline.

Both suites follow the same interface rule: workloads, metric collectors and
result sinks are each an interface with implementations, so adding a workload
or a metric doesn't change the driver.

Both suites:

- run with one command, parameterized by table AM (and pairing), scale and
  client count;
- set up a fresh cluster from a recorded config, then run, collect metrics and
  write results with the raw logs and the exact configuration;
- record the environment (`fio` device baseline, kernel, filesystem, CPU, build
  flags);
- report a stub's "not implemented" error as a clear "not implemented" result,
  not a harness failure.

**Exit criteria:** both suites run end to end; the S suite produces complete
results for heap; the V suite runs against the stubs and reports every
workload as not implemented.

**Status (2026-10-13): built, not yet reviewed.** Harness in
`src/tools/prolly_bench/` (usage in its README; reading notes in
`06-benchmarking.md`, Harness).

| Exit criterion | Result |
|---|---|
| Both suites run end to end | Yes. The quick check (`configs/smoke.toml`) takes about 2.5 minutes for S and 20 seconds for V |
| S suite complete for heap | 70/70 cells OK, S1–S10, every collector except perf (run with an earlier, larger smoke config) |
| V suite reports not implemented on the stubs | 8/8 cells `not_implemented` against `prolly_d1`. The V operations past setup (timed statements, background pgbench) were checked separately against a scratch cluster |

Result sets are kept locally under `specs/results/` and not committed.

Not yet exercised: the perf collector (needs `system/system_prepare.sh`),
and the larger-than-RAM size (open question 18). S10 (sysbench) was added
during step 2 at the user's request.

## 3. Demonstrate the harness and record a local baseline

- Run the S suite against stock heap (B0) on the local machine, at the
  repetition counts in `06-benchmarking.md`.
- Show that results are repeatable: the spread across repetitions is small
  enough to detect the differences we care about. If not, fix the environment or
  procedure before going further.
- Store the baseline under `specs/results/`.

**Exit criteria:** a reproducible local B0 baseline for S1–S9, with its spread
and environment recorded.

**Status (2026-10-13): repeatability shown; full baseline deferred by the
user.** In benchmark mode (`system/system_prepare.sh`), 5 repetitions of S1
and S2 (scale 5, 4 clients, 60 s) had throughput CV 1.2% and range 3.2%, under
the 5% target; latency p50–p99.9 CV 1.1–3.4%. Outside benchmark mode, CPU
boost gave S2 an outlier run (range 8.4%), so benchmark mode is required.
Remaining noise: S1's post-run VACUUM time had one repetition at 4 ms against
44 ms (probably a collision with autovacuum), and device bytes carry the btrfs
fsync cost (open question 20). The full B0 baseline will be run in step 5,
in the same mode and configuration as the engine.

## 4. Implement the first engine

- The first pairing is **D1+K1** (decided 2026-10-13, open question 13):
  immutable copy-on-write on Postgres pages, with a separate lock table. It's
  the simplest engine and the reference point for the others.
- Includes the shared foundation it needs (`04-candidate-designs.md`): node
  page format, chunker, content hashing, key and value encoding, canonical
  export, WAL resource managers, commit publish (B1, B2).
- Includes version-control operations V1–V3 (decided 2026-10-13, open
  question 14); clone, fetch and push stay stubs (when built, clone uses
  implicit TID maps, question 41).
- Schema is versioned with data and commits' root hashes don't depend on history
  (decided 2026-10-13, `09-versioned-catalogs.md` part 1): every catalog is a
  prolly table; the working set is Postgres-native; version-control commits
  are canonical (identity by name, canonical schema derived from the
  catalogs, canonical portable row encoding).
- Decided for this engine: synthetic TIDs with a map (question 2); one
  cluster-wide node store with a hash → block map and per-child block hints
  (question 1); key-only chunk boundaries with a forced split, and SHA-256,
  in our own format (questions 3, 4, starting choices).
- Every piece is an implementation of an interface from step 1; D1 and K1
  must pass their interfaces' conformance suites.

Order of work within the step:

| | Work |
|---|---|
| 4a | Content hash, node format, chunker, key and value encoders. Revisited: a canonical, portable value encoder replaces `heaptuple` as the default, and the key encoder's fallback stops carrying local OIDs (question 12 changed) |
| 4b | Node store and WAL |
| 4c | D1 tree operations |
| 4d | Table and index AM wiring, single writer per table |
| 4e | Concurrency: K1 (replacing single_writer), commit pipeline, root-carrying snapshots (B1, B2); TID counter ranges per backend already, per node if nodes share storage (question 37) |
| 4f | Checkpoint, recovery, reclamation, including TID reuse after VACUUM and gone versions' rows (questions 37, 40) |
| 4g | V1–V3 and versioned catalogs: catalogs as prolly tables, the canonicalizer, the working-set/staged/commit model, refs and bootstrap |
| 4h | S and V suites and correctness gates, including the history-independence gate |

**Status (2026-10-13): 4a built.** Implementations `sha256` (content hash),
`basic` (node format), `keyhash` (chunker), `ordered` (key encoder) and
`heaptuple` (value encoder) pass their conformance suites in
`src/test/modules/test_prolly`; deliberately broken versions of the key
encoder and chunker were checked to fail them. Chunk size targets are
starting values (`chunker_keyhash.c`), to be set by measurement.

**Status (2026-10-13): 4a revisited for canonical form.** Value encoder
`canonical` is the default: live columns only, trailing NULLs omitted,
values in a canonical encoding shared with the key encoder
(`prolly_datum.c`: send formats under a UTF-8 client encoding, OID-free
arrays and composites). The key encoder stores text as UTF-8 and encodes
other types canonically. New conformance checks (independence from DDL
history and from `client_encoding`, columns added with a default, arrays
and composites) pass; `heaptuple` fails the platform and history checks, as
expected. A SQL test shows enum arrays and composites encode identically
after their types are dropped and recreated with new OIDs. A deliberately
disabled encoding override was checked to fail the session check.

**Status (2026-10-13): 4b built.** One cluster-wide node store (`global`:
linear-hashing hash → block map, per-child block hints in internal nodes),
D1's log (`chunks`: full node images), and two built-in WAL resource
managers (`Prolly`, `ProllyStore`). Conformance checks pass, including 5,000
nodes across many map splits; a TAP test replays the store on a standby and
through crash recovery with `wal_consistency_checking`, and a deliberately
broken redo was checked to fail it. Not yet: reuse of released blocks (4f).

**Status (2026-10-13): requirements suite added.** `src/test/prolly` encodes
the decided engine requirements as tests (semantics, concurrency,
durability, version control, canonical form). Its Postgres-semantics groups
pass against heap; against `prolly_d1` every test fails with "not
implemented", which is the work of 4c–4h.

**Status (2026-10-13): 4c built.** A new interface, Tree (`prolly_tree.h`),
holds persistent prolly-tree operations: build, get, cursors in both
directions, batched apply, count, walk and diff. Its implementation `cow`
(D1 and D2) writes copy-on-write trees into the global node store; apply
re-chunks a window around each edit until the new chunking meets an old
node boundary, one level at a time, so an edit rewrites one path plus the
nodes next to it. The conformance suite checks get and scans against a
reference map, random batches of edits, history independence (the same
items give the same root hash whatever the order and batching of builds
and applies, and emptying a tree gives the empty tree's hash), node
sharing after an edit, diff, and page validity. A regression check covers
edits past the last key when the last leaf ends on a chunk boundary, which
once made apply loop forever; apply now also raises an error if a window
makes no progress. Deliberately broken versions (the old loop, no root
collapse, an off-by-one backward seek, a diff that drops modifications,
wrong subtree counts) were checked to fail the suite. Not yet: values too
large for a node (an error for now), freeing replaced nodes (4f), and the
root layout of question 33, which comes next as its own change, built on
trees, before 4d.

**Status (2026-10-13): root layout built.** A new interface, Root layout
(`prolly_rootlayout.h`), says how a branch's root finds every relation's
tree. Its implementation `subroots` (D1, D2) makes the working-set root a
small tree of two sub-roots, catalog and user data, each a tree mapping
relation OIDs to relation-tree hashes (one entry per relation; which
sub-root is the caller's choice). Operations: empty, lookup, apply, subroot
(for one-comparison "did the schema change?"), scan, walk (into every
relation's tree, for reclamation in 4f) and diff (skipping an unchanged
sub-root). Its conformance suite checks lookups and scans against a
reference, history independence, sub-root independence, diff and walk; five
deliberately broken versions were checked to fail it. The canonical root,
keyed by name, comes with the canonicalizer in 4g. Code comments no longer
refer to these specs (user rule, 2026-10-13): specs may point at code, not
the reverse.

**Plan (2026-10-14, user): 4d in four stacked PRs.**

| | Work |
|---|---|
| 4d.1 | Branch head store (one working-set head per database, in shared memory, WAL-logged, saved at checkpoint); D1 private writes per transaction with command-id and savepoint overlay; insert and sequential scan; tables keyed by primary key or by content (question 36); local root by relfilenumber with a local sub-root (question 33) |
| 4d.2 | TID maps (question 37); fetch by TID, delete, update, row locks for a single writer; btree and the other index AMs on prolly tables |
| 4d.3 | The prolly index AM |
| 4d.4 | Remaining table AM callbacks: ANALYZE, sampling, bitmap scans, CLUSTER, size estimates |

Until 4e, every snapshot reads the latest published head (no root-carrying
snapshots), and a table has one writer at a time. Values too large for a
node stay an error until out-of-line storage.

**Status (2026-10-14): 4d.1 built.** Engine D1 stores tables: private
writes per transaction (command-id and savepoint visibility, Halloween
protection), commit through a new commit pipeline implementation
`serial` (branch heads in shared memory, a `ProllyCommit` WAL record per
new root, `global/prolly_heads` at checkpoints, heads settled through the
commit log after a crash or on a standby), a new interface Table keying
(`prolly_keying.h`, implementation `pk_or_content`, question 36) and row
identity `hashed` (question 37). The local root keys relations by
relfilenumber and has a local sub-root (question 33 changed), so
transactional TRUNCATE and table rewrites behave as on heap. Each data
tree records its keying and row layout; a primary key added or dropped,
a rewrite (which goes through a table without one) or a dropped column
rebuilds the tree at the next write, keeping every row's TID (re-encoded
rows are renamed in the TID map). Bug found in 4a's `canonical` value
encoder: rows written before a DROP COLUMN (which doesn't rewrite the
table) couldn't be decoded; value encoders now describe their row layout
and decode rows from earlier layouts, with a conformance check and SQL
tests. Core changes: a prolly shared-memory
subsystem and checkpoint hook, a `prollybranch` lock tag, the
`ProllyHeads` LWLock, the `ProllyCommit` resource manager, and optional
parallel-scan callbacks (a table AM without them is scanned only in the
leader, like temporary tables, because workers can't see the leader's
private writes). Table AM: sequential scans, inserts, size estimates,
index builds; TID fetches, updates, deletes, row locks and index scans are
4d.2. Tests: conformance suites for engine D1, `serial`, `hashed` and
`pk_or_content` (`src/test/modules/test_prolly`), SQL tests of D1 tables
(`d1_tables`), and a TAP test of heads across crashes, checkpoints,
restarts, standby replay and promotion (`002_d1_heads.pl`).

Review fixes (2026-10-14): heads are copied by CREATE DATABASE (both
strategies) and forgotten by DROP DATABASE, through two new commit
pipeline callbacks, `copy_database` and `drop_database`, and every prolly
relation gets an empty main fork; keying descriptions record key column
types, so a dropped primary-key column's rows stay readable; D1 cleans
up its transaction state after a PREPARE and after a lock error on a
table's first write. A copy's pending heads are published or dropped
when its transaction ends, and checkpoints on a primary settle pending
entries nobody else will (their xids are invisible to commit-log
truncation and wraparound); restartpoints don't. A `usage` callback
reports a pipeline's heads and pending entries. A full heads table
(1024 database-branch pairs across the cluster) used to PANIC after the
commit record, and again on every replay; a commit needing a new head now
reserves its slot when it enqueues and fails with an ERROR instead
(`debug_prolly_max_branch_heads` lowers the limit for tests,
`006_d1_head_limit.pl`). Redo settles a branch's previous entry before
adding the next and makes room from replayed commits and aborts, so
recovery replays every accepted commit at the real 1024-entry capacity
(`007_d1_commit_capacity.pl`). Keyings gained `can_compare`: after a key's
type is dropped D1 matches keys by bytes, so a transaction's own rows
are still read, rebuilt and committed. Each has tests
(`003_d1_cleanup.pl`, `004_d1_databases.pl`, `005_d1_pending.pl`,
`103_database_copy.pl`, `sem_ddl`, `sem_transactions`).

**Status (2026-10-14): 4d.2 built.** prolly_d1 tables support fetching by
TID, UPDATE, DELETE, row locks, ON CONFLICT, TID and TID-range scans,
plain and index-only index scans with btree and the other index AMs,
CREATE INDEX CONCURRENTLY, and xmin. Built:
- Coordination implementation `single_writer` (question 38): a per-table
  write lock of its own (`prollytable` in pg_locks), exclusive for writes
  and FOR [NO KEY] UPDATE, shared for FOR [KEY] SHARE. It replaces 4d.1's
  relation ExclusiveLock, which conflicts with the RowExclusiveLock every
  writing statement holds, so two writers that had both started
  deadlocked.
- Row identity `counter` (question 37 changed, user): per-table counter
  TIDs in the main fork, never reused after aborts, retired TIDs with
  successors (question 39) and the gone version's row (what SnapshotAny
  returns), xmin per version. It replaces `hashed`: a row inserted again
  after a rollback got its aborted TID back, and indexes returned it
  twice.
- Table AM: update/delete/lock classify a row as live, changed by this
  transaction, or changed by one that committed while it waited
  (TM_Updated with the successor, followed by READ COMMITTED);
  speculative insertion; a prolly slot type (xmin,
  is_current_xact_tuple); `index_validate_scan`.
- Engine contract: writes tagged InvalidCommandId (rebuilt rows) are seen
  by every command; D1 readers see their own root set with the
  transaction's writes over it, not the write set's newer base.
Bugs found and fixed, each with a test that fails without the fix: the
concurrent-writer deadlock; TID reuse after abort; a waiting writer read
a stale root set (a committed row was overwritten by a duplicate, and an
update was applied to an old version, losing a concurrent one); D1 cursors
mixed root sets; upserts didn't wait for a concurrent insert of the same
key; PK-keyed trees silently replaced a row on a duplicate key (now
btree's error); dirty snapshots were left uninitialized; rebuilt rows were
invisible to the command that rebuilt them; ALTER COLUMN TYPE with data
decoded old rows with the new types (4d.1); bottom-up index deletion
asserted. One test was made deterministic (an ORDER BY with ties in
`sem_indexes`). Requirements suite against prolly_d1: sem_ddl,
sem_constraints, iso_unique_wait and iso_read_committed pass; the rest
fail only on values larger than a node, PREPARE after prolly writes,
temporary tables, the 4d.4 callbacks (ANALYZE, bitmap scans, CLUSTER),
REPEATABLE READ and SERIALIZABLE (newest-head snapshots, 4e), and the
single writer's coarser blocking (4e). Not tested: index_validate_scan
with rows written while the index is built.

**Status (2026-10-14): 4d.3 built.** The prolly index AM works on prolly
tables (decisions: questions 17, 42, 43). A new interface, Index keying
(`prolly_ixkeying.h`, implementation `columns_then_row`), makes an entry from
the index columns, then the row's key and ordinal, with no value and no TID,
and bounds for seeking; the key encoder gained `prefix_length` (encodings are
self-delimiting). D1 orders an index's tree by its index keying. Built:
builds (refusing duplicates for unique indexes), inserts with unique checks,
removal of a row version's entries by the table AM in the same write (with
expressions and partial predicates), rebuilds when the table is re-keyed,
ordered scans in both directions bounded by leading equality keys and a range
key (DESC columns bound the other end), index-only scans, mark and restore
(merge joins), ON CONFLICT arbiters, REINDEX, CREATE INDEX CONCURRENTLY,
TRUNCATE, crash recovery and standby replay. Prolly indexes on heap tables
are refused. Tests: conformance checks for index keying and key prefixes,
`d1_index` (every index agrees with sequential scans, and its tree equals a
freshly built index's after updates, deletes, rollbacks, keyless deletes and
re-keying), `009_d1_index.pl`. Not yet: bitmap scans (built in 4d.4), INCLUDE columns,
parallel scans and builds, predicate locks; deferrable unique constraints
can't use prolly indexes (Postgres requires btree for constraint indexes).
Scan bounds only narrow the scan (every key is checked on each entry), so
their stop condition has no test that fails without it.

**Status (2026-10-14): 4d.4 built.** The remaining table AM callbacks
(decision: question 44).
- ANALYZE: a new optional table AM callback, `relation_acquire_sample_rows`
  (`tableam.h`, used by `analyze.c` for tables and inheritance children;
  heap leaves it NULL). The tree interface gained `key_at` (the key at a
  position, from subtree counts), the engine interface `key_at` and
  `has_writes`. For a table with a primary key that the transaction hasn't
  written, the prolly sampler picks distinct positions (selection when the
  table is under twice the sample, else random draws redrawn for repeats)
  and reads only those items. Keyless tables (decided by the user
  2026-10-14: an item holds identical rows, so picking items would rarely
  see a value that is most of the table) and tables the transaction has
  written (positions don't cover its writes) are read whole into a reservoir
  over rows. The sample is returned in key order.
- TABLESAMPLE: pseudo-blocks of 64 items in key order, found by position, so
  SYSTEM reads only the blocks it picks; if the transaction has written the
  table, the blocks are cut while reading it. A block with more rows than a
  page has offsets is shown to the method a page at a time, each chunk under
  its own block number.
- Bitmap scans: the table AM looks up each TID of the bitmap (all of a lossy
  page's, rechecked); the prolly index AM has `amgetbitmap`.
- CLUSTER and VACUUM FULL: the new storage takes the table's trees and TID
  counter as they are (O(1); rows are in key order already), so trees,
  digests and TIDs are unchanged; the row count reported sums identical rows.
  If columns were added with a default, the rows are rewritten with their
  values (Postgres clears the catalog's missing values after a rewrite),
  through a new table keying callback, `open_rewritten`; they keep their TIDs
  and match a table created with the column.
- Size estimates: pages from the item count and the columns' type widths
  (stable across ANALYZE); rows from the rows per page last recorded, as
  heap estimates, so keyless tables with identical rows are planned by rows.
  A new core helper, `table_relation_stats_pages`, makes ANALYZE, CLUSTER and
  index builds record the estimate's pages for such an AM.
Bugs found and fixed, each with a test that fails without the fix: the
planner counted a keyless table's items, not its rows, and VACUUM FULL
recorded that count; a bitmap index scan, which isn't given the table,
crashed. Found in review: CLUSTER and VACUUM FULL lost the values of
columns added with a default; ANALYZE of a table written in the transaction
found no rows or miscounted them; keyless tables with a dominant value were
sampled with huge variance; CLUSTER, VACUUM FULL and CREATE INDEX recorded
zero pages, losing the planner's row density; chunks of a large sample
block repeated BERNOULLI's choices. Tests: `key_at` in the tree conformance checks, `sem_sampling` in
the requirements suite (statistics, TABLESAMPLE, bitmap scans with lossy
pages, CLUSTER and VACUUM FULL, against heap's output), `d1_maintenance`,
`010_d1_maintenance.pl` (what ANALYZE reads; CLUSTER and VACUUM FULL across
a crash and on a standby). Requirements suite against prolly_d1: the
semantics group fails only on values larger than a node, PREPARE after
prolly writes and temporary tables (`REFRESH MATERIALIZED VIEW
CONCURRENTLY`).

**Exit criteria:** the S and V suites run against D1+K1, the requirements
suite (`src/test/prolly`) passes against it, and the correctness gates in
`06-benchmarking.md` are run, with their status recorded.

## 5. Measure the first engine

- Run the full B0 baseline (deferred from step 3) and both suites against
  D1+K1, on the same machine, in benchmark mode, with the same configuration.
- Collect profiles (`perf`) for the workloads where D1+K1 differs most from B0.

**Exit criteria:** S results side by side with B0 (normalized to B0 = 1.0), V
results, gate status, profiles; stored under `specs/results/`.

## 6. Discuss what we observe and why

- Write up what the numbers show and **why**, tracing each significant
  difference from B0 to a cause in the design or implementation, using the
  profiles and engine counters.
- State which hypotheses in `04-candidate-designs.md` the results support or
  contradict.
- Bring that to a discussion with the humans. The roadmap stops here; what to
  build next is decided then.
