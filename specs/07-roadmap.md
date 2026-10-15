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

**C suite (2026-10-15, user request):** a third suite, SQL correctness,
added after step 2: the sqllogictest corpus against heap and each engine,
judged by agreement with heap (`12-correctness-suite.md`, gate 8 in
`06-benchmarking.md`). Building it found that `--am prolly` had been
measuring heap in S and V; they now create prolly's tables in a versioned
schema.

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
| 4e | Concurrency: root-carrying snapshots first (B1, B2: every scan of a snapshot reads the same root set); K1 row-level locks replacing single_writer, so writers to different rows of one table run concurrently, as on heap; a commit pipeline that materializes changed trees before taking the branch's commit lock and, under it, replays the transaction's write set onto a newer head (row locks keep concurrent writers off each other's rows, so replay needs no merge); bounded transaction memory: write sets flushed to intermediate trees, with savepoint roots, their edits recovered at replay by tree diff (question 47); tree diff that skips unchanged internal subtrees, not only leaves (needed by replay; also status, diff, merge and transfer); engine counters of nodes read and written, so tests can check complexity bounds (gate 7); TID counter ranges per backend already, per node if nodes share storage (question 37) |
| 4f | Checkpoint, recovery, reclamation, and large values: out-of-line value trees (bytes split by a rolling hash) whose references the node format exposes to walk, reclamation and transfer (question 48); reclamation from the full set of live roots (question 49), including TID reuse after VACUUM and gone versions' rows (questions 37, 40) |
| 4g | V1–V3 and versioned catalogs: first, the canonical format freeze (provisional until every engine implementation has been evaluated: step 5 and later may still change it; a chunk rule independent of `BLCKSZ` with size targets set by measurement, question 45; portable codecs or refusal for `reg*` and extension types, question 35; collation versions, question 46); then catalogs as prolly tables, the canonicalizer, the working-set/staged/commit model, refs and bootstrap, and three-way merge that reuses unchanged subtrees |
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

**Review against Dolt and DoltLite (2026-10-14).** Two source-level
comparisons of this implementation with Dolt and DoltLite (kept outside this
repository) found the same tree model in all three, with the gaps in the
machinery around it. Taken into the plan above (user, 2026-10-14): snapshot
roots and row-level locking first (4e); bounded transaction memory and
subtree-skipping diff (4e); large values with a reference contract (4f);
the full set of roots reclamation protects (4f); the canonical format freeze
before the first canonical commit (4g); and benchmarks that separate
canonical bytes, physical pages, TID-map bytes and WAL (`06-benchmarking.md`).
Structural sharing between the working set and commits is by design: data
and index trees are canonical as written, and a commit's canonical root
refers to the same trees (`09-versioned-catalogs.md`). Bug found while
checking one finding, fixed with tests that fail without the fix: an update
of a key (or a prolly-indexed column) to an equal value written differently
(numeric 1.0 to 1.00, or 'a' to 'A' under a case-insensitive collation)
failed with a duplicate-key error, and D1's write set kept the old key
bytes, so an index-only scan returned the old spelling and the trees
differed from ones written with the new value directly (the tree's diff now
reports such a key as modified).

**Complexity requirement (user, 2026-10-14).** Every version-control
operation must at worst match Dolt's time and space bounds, ideally beat
them (`10-complexity-requirements.md`). Two are not met today and are fixed
in 4e: row diff (leaf walk, O(N / leaf size), instead of O((Δ + 1) log N))
and pending-write memory (unbounded). The rest are met by design and
checked when built (4g, V4); engines expose node read and write counters so
tests can assert the bounds (gate 7).

**Plan (2026-10-14, user): 4e in five stacked PRs, riskiest dependencies
first.**

| | Work |
|---|---|
| 4e.1 | Engine counters of nodes read and written; tree diff that skips matching internal subtrees, O((Δ + 1) log N), with a counted test of the bound (gate 7) |
| 4e.2 | Root-carrying snapshots: a snapshot keeps the root set it started with, so every scan of a REPEATABLE READ transaction reads the same roots (B1, B2) |
| 4e.3 | K1 row-level locks replacing `single_writer`, so writers to different rows of one table run concurrently; the commit pipeline materializes changed trees before the branch's commit lock and, under it, replays the write set onto a newer head |
| 4e.4 | Bounded transaction memory (question 47): write sets flushed to intermediate trees with savepoint roots, edits recovered at replay by diff; PREPARE TRANSACTION after prolly writes, its write set flushed and its root recorded in the two-phase state file, replayed at COMMIT PREPARED (user, 2026-10-14) |
| 4e.5 | SERIALIZABLE on prolly tables; the isolation-suite stalls from the single writer; temporary and unlogged tables use heap (question 28); TID counter ranges per backend |

**Status (2026-10-14): 4e.1 built.** The tree interface counts nodes:
a caller that sets `counters` in its tree environment gets one per node
read from the node store and one per new node written (not one the store
already had). The engine interface gained `node_counters`, D1 counts every
tree it uses, and `prolly_node_counters(access_method)` returns a backend's
counts. The tree's diff walks both trees with cursors that stand on whole
subtrees, judged by the child hash in the parent's item: equal subtrees at
equal heights are skipped unread, the taller side descends, and leaf items
merge by key, so a diff reads O((Δ + 1) log N) nodes. Tests: conformance
checks that the counters count builds, rebuilds and walks exactly; that a
diff of d spread changes reads at most d(4 levels + 4) nodes (measured: 8
for one change in a 4-level tree of 837 nodes, 40 for eight; the leaf walk
read 1,674 for one, and fails the check), nothing for a tree against
itself, and every node once against the empty tree; diffs across trees of
different heights and of unrelated trees; and, in `d1_tables`, that a
primary-key lookup and a one-row update of a 20,000-row table read and
write a few paths. Found by the counters: a sequential scan of a prolly
table found each row's TID with a point lookup in the row-identity tree,
reading O(N log N) nodes (about six per row on a 20,000-row table). Fixed
in a follow-up PR before 4e.2: a tree cursor's seek keeps the nodes of the
path it holds, so seeks in key order read each node about once; a D1
cursor's seek sees writes made since it opened, as a lookup does; and a
scan finds TIDs by seeking one cursor over the version entries. The
version entries moved to their own local tree, ordered as the data tree
is (`PROLLY_TREE_ROWKEY`, in a keyed local sub-root), so the seeks move
forward for every key type: collated text, numeric and other keys whose
bytes don't sort in key order included (a review found 40,726 reads for an
ICU-collated scan of 20,000 rows when the entries were in byte order; now
1,384). The scan reads each node of the data tree and of the version
entries once (1,358 reads for 20,000 integer keys, 1,437 for numeric ones;
checked in `d1_tables`, and seeks in order by a tree conformance check).
While a table's key order is lost, a scan reads the version entries whole
and looks them up by bytes, until the next write rebuilds them.

**Decisions for the rest of step 4 (user, 2026-10-14).** Every open choice
the remaining work reaches was settled as recommended (questions 23, 25,
27, 29, 34, 35, 46–56): K1 locks by (relation, key hash) on the lock
manager; one commit lock per branch; `prolly.write_set_memory`; values out
of line past about 2 KB, uncompressed in canonical form; an explicit
`prolly_gc()`; tablespaces ignored for prolly tables; Doltgres's SQL
interface (`dolt_*` functions and system tables, `dbname/branch`);
versioned databases by opt-in; shared catalogs unbranched; permanent heap
tables refused in a versioned database; Dolt's conflict tables; renames
merged as drop and add; a pinned canonical deparse; `dolt.author`.

**Status (2026-10-14): 4e.2 built.** An MVCC snapshot reads the root of
the newest commit it sees as committed, decided by xids as heap tuples'
visibility is: the commit pipeline gained `snapshot_head`, which looks at
roots enqueued by transactions the snapshot sees as committed (published or
not yet), then the head, then a history of replaced heads, each with the
xid that committed it and the xid that replaced it. Commits to a branch end
in enqueue order (the branch lock is held to the end of the transaction),
so a snapshot that sees one commit sees every earlier one. History entries
are dropped once every running snapshot sees their successor (the oldest
xmin passed it); if the history (4,096 entries) is full of entries still
needed, the oldest is given up and a snapshot that needed it gets
"snapshot too old" (a serialization failure, so clients retry). Standbys
keep their own history from the commits they replay. Snapshots other than
MVCC (dirty, self, any) and writes read the newest head, as before. No
change to `SnapshotData`: exported, imported and parallel snapshots carry
their xids, which is all the choice needs. Tests: `012_d1_snapshots.pl`
(REPEATABLE READ keeps its root through sequential and index scans, also
when the snapshot was taken by a heap table first; READ COMMITTED moves
on; an update of a row changed since the snapshot is a serialization
failure; history pruning and "snapshot too old" with
`debug_prolly_max_root_history`; a standby snapshot), which fails without
the change (each scan read the newest commit); a commit pipeline
conformance check that a snapshot doesn't see a running transaction's
roots.

**Status (2026-10-14): 4e.3 built.** Coordination K1 replaces
`single_writer` for `prolly_d1`: a row is locked by a heavyweight lock on
(database, table, 48 bits of a 64-bit hash of its key) (`LOCKTAG_PROLLY_ROW`,
`prollyrow` in `pg_locks`; the tag has room for 48 bits of the hash,
question 50), in the lock mode heap uses for a tuple lock of the same
strength, so the four row lock modes conflict as on heap. Every row lock
first takes an intent lock on the table's own lock (`prollytable`), which
whole-tree writes (rebuilds) take in conflicting modes. Past
`prolly_max_row_locks_per_table` (64) row locks on one table, a transaction
locks the whole table instead, which bounds the lock manager's memory.
A key's bytes name its row only if keys that compare equal are the same
bytes (a new `equal_bytes` callback on the key encoder, table keying and
index keying, from btree's `equalimage` support functions); tables keyed
otherwise (numeric, nondeterministic collations) lock whole tables.
Updates that keep every column a foreign key can reference take FOR NO
KEY UPDATE, as heap's do, so foreign-key checks aren't blocked. Prolly
unique indexes lock their key values, so inserters and removers of one
value take turns. Uniqueness checks of other index AMs (btree) wait for a
row's writer the way heap's do: the table AM reports the writer
(coordination's new `writer_of`) in the dirty snapshot when the caller holds
its page, and waits itself otherwise. At commit, D1 builds every changed
tree before taking the branch lock; under it, a tree another transaction
changed meanwhile gets the transaction's edits replayed onto the tip's
version (no merge: row locks kept the writers apart). Bugs found and fixed,
each with a test that fails without the fix: a write or row lock on a row
updated meanwhile locked the whole table and deadlocked with row lockers
(the lock path now follows the update chain and locks the new version's
key); a uniqueness check waited for the other writer while btree held its
leaf page, so lock timeouts and deadlocks went unseen. Tests:
`013_d1_row_locks.pl` (writers of different rows run at once and both
commits are kept, with their index entries; one row, one primary key and
one unique value, btree's or a prolly index's, take turns; foreign-key
checks and no-key updates; escalation; numeric keys; deadlocks; 600
concurrent transfers keep every row and the total), `008_d1_writers.pl`
and `003_d1_cleanup.pl` updated for row-level locks, and a key encoder
conformance check of `equal_bytes`. The requirements suite's isolation
group now passes except SERIALIZABLE (4e.5): row locks, unique waits,
REPEATABLE READ conflicts, foreign-key key-share locks, READ COMMITTED
re-checks, deadlocks and concurrent writers. Not yet: group commit.

Later (after 4g.3): a btree uniqueness check that met a TID nobody could
resolve (another writer's private row, or an aborted insert's) waited for
every writer of the table, which deadlocked concurrent transfers on
`013_d1_row_locks.pl` now and then. Writers now lock the values their rows
have in unique indexes of other access methods too, on write and delete,
as a prolly unique index locks its own (by bytes when equal values encode
alike, else the whole index). A btree check, which holds that lock for
its own value, takes such a TID as gone; checks from the executor (ON
CONFLICT, exclusion) still wait for the table's writers. Test: an aborted
insert's entry no longer makes a later insert of the same value wait for
another row's writer (fails without the change).

**Status (2026-10-14): 4e.4 built.** A transaction's private writes stay
bounded (question 47): past `prolly_write_set_memory` (64 MB; named with
an underscore, as core settings have no dots), D1 spills every write set
into a spill tree in the node store and empties it in memory. A spill
tree is a delta, not a rebuilt table: keyed as the write set, each value
the key's newest version and, if it differs, the newest one the current
command may see, with their command ids, so even one statement that
writes more than the limit (and reads the table it writes) spills, which
the plan's "versions older than the current command" would not have
allowed. Reads merge memory, the spill tree and the base; commit streams
the spill tree and memory onto the base (or the tip) a chunk at a time,
so commit needs no more memory than a spill. Each subtransaction open at
a spill gets a mark, the spill tree without its versions, which rolling
back to it restores (a new core function lists the open subtransactions).
A command that began before a spill (an open cursor) and would need
versions the spill dropped gets an error, rather than wrong rows.
PREPARE TRANSACTION with prolly writes works: each written tree is built
as the transaction leaves it and recorded in the two-phase state (base and
final roots, key order); the transaction keeps its row locks across a
crash as any lock; COMMIT PREPARED replays the difference from the base
onto the branch's tip (tree diff, so O(changes)), under the branch lock,
before the commit record, and enqueues the root under the prepared xid
(the commit pipeline's `enqueue` now takes the xid). Core changes: a
two-phase resource manager for prolly (`TWOPHASE_RM_PROLLY_ID`),
`AtPrepare_Prolly()`, pre-commit two-phase callbacks, and
`GetOpenSubTransactionIds()` (`02-postgres-integration.md`, B5, B12).
Found and fixed, each with a test that fails without the fix: a new
table's first writers locked the whole table to record its keying, so
they ran one at a time (a table with no rows now records it without the
lock); after each spill, the emptied write set reopened its key order in
transaction memory (8 MB leaked in a 30,000-row insert) and could have
reordered a spill tree. Tests: `014_d1_spill.pl` (memory stays under
2 MB with a 64 kB limit; reads, index scans, savepoint rollbacks,
self-reading inserts and a concurrent commit around spills; the cursor
error; PREPARE across an immediate restart, row locks kept, COMMIT
PREPARED replayed onto others' commits, ROLLBACK PREPARED, a spilled
prepared transaction on a standby); `013_d1_row_locks.pl` gained the
new-table case. Not yet: write sets of tables whose key order was lost
don't spill; versions saved when a write set is emptied (TRUNCATE in a
transaction) stay in memory; spill and prepared trees are garbage once
their transaction ends, for reclamation (4f) to free.

**Status (2026-10-14): 4e.5 built; 4e is done.** SERIALIZABLE works on
prolly tables, at table granularity: a read records a predicate lock on
the whole relation (K1's `predicate_lock_range`), a write checks for
others' read locks on it, and a reader finds its conflicts with the
writers it doesn't see, as heap finds them per tuple from xmin and xmax:
those still running (coordination's new `current_writers`, from the
table's write locks) and those that committed after its snapshot (the
engine's new `unseen_writers`, from the commit pipeline's new
`commits_since`: the commits the snapshot doesn't see whose roots changed
the table's tree). Coarser than heap, so more transactions fail to
serialize, never fewer; key-range predicate locks are later work
(`02-postgres-integration.md`, A4). Temporary and unlogged tables get heap
when a prolly access method is the default (question 28); an explicit
`USING prolly` on one is refused, and a concurrent refresh of a prolly
materialized view builds its temporary diff table on heap. The isolation
stalls of the single writer went with K1 (4e.3), and TIDs were already
reserved per backend (1,024 at a time, 4d.2). Tests:
`015_d1_serializable.pl` (write skew in the three orders reads and writes
can come in, two of which fail without the conflict-out checks;
transactions on different tables commit; temporary and unlogged tables;
the concurrent refresh). The requirements suite's isolation group now
passes in full, and its semantics group fails only on values larger than
a node (4f).

**Plan (2026-10-14): 4f in three PRs.** 4f.1 large values; 4f.2
reclamation of nodes (`prolly_gc()`, reusing freed blocks); 4f.3 VACUUM of
gone versions, and TID reuse. Checkpoints and recovery were built in 4b
and 4d and are covered by the durability suite.

**Status (2026-10-14): 4f.1 built.** Values larger than a node work
(question 48). The tree stores a leaf value longer than 2 KB out of line,
in a value tree: its bytes cut where a buzhash rolling hash (48-byte
window, table fixed by the format) says so, between 512 bytes and 4 KB,
about every 2 KB, keyed by offset. The leaf item holds the value tree's
root hash and the value's length, flagged in the node format (the top bit
of the item's value length, in the canonical region), so walks, and
reclamation and transfer through them, find every value tree. Get,
cursors and diff return the bytes; diff compares references, so equal
large values cost nothing. Canonical bytes are uncompressed. Every tree
gets this, so rows, the row-identity map's gone versions and spill trees
all hold large values. The row-identity map now keeps a gone version's
row as its own item ('G'), the same bytes the data tree held, so a large
row's value tree is shared rather than written again (format version 3).
Found and fixed, with a test that fails without the fix: updating a few
bytes of a 192 KB value wrote 59 nodes, because the gone version's row
was stored behind a header and its offsets shifted (now under 40, most of
them the row-identity map's and data tree's own paths). Tests: a tree
conformance check (`large_values`: round trip through get, cursors and
diff, history independence of trees built at once and by edits, walks
reaching value trees, a one-byte change writing at most 12 nodes);
`016_d1_large_values.pl` (rows up to 1.3 MB, keyless tables, a crash, a
standby). The requirements suite's semantics group now passes in full.
Not yet: keys by offset mean an insertion in the middle of a large value
rewrites the value tree after it; Dolt's value trees, which are addressed
by byte counts rather than keys, avoid that, and step 5 settles the
format.

**Status (2026-10-14): 4f.2 built.** `prolly_gc()` reclaims the node
store's unreachable nodes (question 49; decided: explicit, as `dolt gc`).
A new reclamation implementation, `mark_sweep`, marks every node reachable
from a root the commit pipeline holds (its new `live_roots`: every
database's branch heads, the roots older snapshots read, roots enqueued
for commit; history no snapshot needs is pruned first), through the root
layout into every relation tree and, through the node format's flags,
into value trees; a walk skips what lies below a node already marked (the
tree interface's walk visitor now says whether to descend), and marks are
bits by block. It then sweeps what the node store can find by hash and
isn't marked (the node store's new `scan` and `reclaim`). A reclaimed
node's block is recorded in a bitmap of free blocks (a fourth store file)
with the transaction id at the time, and reused by `new_node` once every
transaction older than that has ended, as nbtree recycles pages; a hot
standby cancels older queries when it replays the reuse. The node page
itself is left alone until then, since a reader may still hold a pin on
it. Nodes a transaction holds only in memory (trees it builds, spills or
has prepared) are kept safe by a new heavyweight lock, `prollystore`:
writers take it shared from their first node written or found until they
end, `prolly_gc()` exclusively, so it waits for them (prepared ones too)
and holds new ones off. Running it needs the privileges of `pg_maintain`.
Tests: `017_d1_gc.pl` (nodes of replaced and dropped trees and of large
values freed; current rows intact; an older REPEATABLE READ snapshot still
reads its root, whose nodes go once it ends; freed blocks reused instead
of growing the store; waiting for a writer with nodes written and for a
prepared transaction; a crash; a standby; privileges). Not yet: blocks
written but never published (a crash between the two) aren't found;
reclamation runs only when asked; `prolly_gc()` holds every writer off for
its whole run.

**Status (2026-10-14): 4f.3 built; 4f is done.** VACUUM of a prolly
table removes row versions no snapshot can still see, as heap removes dead
tuples (questions 37, 40). The row-identity map's gone entries now record
the transaction that retired them (format version 4); VACUUM asks it for
the TIDs retired before the table's oldest non-removable xid
(`dead_tids`), removes other index AMs' entries for them with their bulk
delete (a prolly index's entries point at row keys and go with their
rows), then forgets the TIDs (`tid_forget`): their map entries go with
VACUUM's transaction, and their numbers go on a free stack in the
table's main fork, each with VACUUM's xid. New versions take numbers off
the stack before reserving new ones, but only those whose VACUUM has
committed: until then its removal of the map entries isn't in the branch,
and a reuse committed first would have its entries removed by VACUUM's
replayed commit. Found and fixed: the free stack sized itself by the table
AM's size estimate rather than the file; `010_d1_maintenance.pl`'s
standby lost WAL now that VACUUM writes (it has a replication slot now).
`017` and `018` run with WAL consistency checking. Tests: `018_d1_vacuum.pl`
(VACUUM keeps what an open snapshot sees; afterwards new rows reuse freed
TIDs, and no index entry of a removed version reaches a reused TID, which
fails without the index cleanup; a crash; a standby). Not yet: index
entries of inserts that aborted (their TIDs never named a committed
version) stay until the index is rebuilt; once branches exist (4g), a TID
gone on one branch may still name a version on another, so VACUUM must
consult every branch before forgetting (done 2026-10-15: TIDs a commit or
branch may hold aren't reused, below).

**Plan (2026-10-14): 4g in stages.** 4g.1 refs, branches, commits and
checkout, with data versioned per branch (catalogs still shared heap
tables); 4g.2 diff and three-way merge; 4g.3 the canonicalizer: commits'
root hashes from canonical schema and data, by name; 4g.4 Doltgres's SQL interface (`dolt_*` functions and
system tables, `dbname/branch`); 4g.5 versioned catalogs (every catalog a
prolly table: `09-versioned-catalogs.md`, B10), the largest piece, after
which schema is per branch.

**Status (2026-10-14): 4g.1 built.** A refs implementation, `tree`,
keeps a database's branches and commits in a prolly tree that is the head
of a reserved branch of the commit pipeline (`PROLLY_REFS_BRANCH`), so
refs are transactional, WAL-logged, crash-safe and replicated as working
sets are, without a refs file (question 32's proposal, met another way).
The refs tree maps branch names to branch numbers and head commits, and
commit ids to the working set each recorded; a commit is a small tree of
its own (author, message, parents, root, time), its id the root's hash.
Writers of refs take the refs branch's lock before reading what they
change, so branch numbers are given out once. Each branch has its own
working set, a head in the commit pipeline; a session's checkout picks
which one its transactions read and write (D1 takes the branch when a
transaction first writes; a checkout with uncommitted writes is refused),
and a prepared transaction commits to the branch it wrote. K1's locks
include the branch, so writers of one row on different branches don't
wait for each other. `prolly_commit()` records the branch's working set;
`prolly_branch_create()` starts a branch from a commit's working set (or
the current branch's head commit), `prolly_checkout()` switches.
Reclamation keeps what refs reach: the refs tree, every commit, the
working set it recorded and its root (the refs interface's new `walk`;
the commit pipeline's `live_roots` now says which branch a root is from).
Tests: `019_d1_branches.pl` (rows per branch; row locks per branch; a
prepared transaction's branch; a branch from an old commit after
`prolly_gc()`; a crash; a standby; unknown branches). The requirements
suite's `vc_branches` and `vc_branch_sessions` now pass (the latter's
setup steps now print nothing: a commit id differs from run to run). Until 4g.2 a commit's root is its
working set's local root, so root hashes depend on history; until 4g.5
schema is shared by every branch (a table created on one branch exists,
empty, on the others).

**Status (2026-10-14): 4g.2 built.** A diff and merge implementation,
`rows`. Diff compares, for every prolly table, its data tree in the
working sets two commits recorded (the engine's new `diff`, which skips
equal subtrees; `roots_at` opens a root set at any working set). Merge is
three-way from the newest common ancestor of the current branch's head
commit (`HEAD`, which refs now resolve) and the other branch's: for each
table, the other side's changes since the ancestor are applied as row
writes through the executor, as logical replication applies changes, so
row identity, every index (btree and prolly alike), constraints and
triggers see ordinary writes. A key both sides changed alike is left
alone; changed differently, it is a conflict, and the merge fails with
nothing changed (Dolt's conflict tables, decided in question 55, are not
built yet). Copies of a keyless table's rows add up. The merge holds the
branch's commit lock from the start, so the working set its commit
records (the engine's new `pending_root`) is the one the transaction
commits; the commit has both heads as parents (refs' new `commit_with`;
`commit_working_set` gives a commit's working set). A branch with changes
since its head commit isn't merged into; a merge already done is a no-op.
Tests: `020_d1_merge.pl` (diff by kind; a merge of updates, deletes,
inserts and large values on both sides; btree and prolly indexes after
it; keyless copies; merging again; a conflict that changes nothing; a
branch with uncommitted changes; a crash). The requirements suite's
`vc_diff_merge` now passes.

**Status (2026-10-14): 4g.3 built.** A canonicalizer interface (the
21st) and its implementation, `catalogs`: a commit's root is now the
canonical form of the working set it records, so its hash depends only on
what it holds (gate 6). The canonical root is a tree by kind and qualified
name: prolly tables (their data tree, then their schema: live columns in
order with types, nullness, collations, defaults, identity and generation;
constraints, indexes, triggers and rules by name, as Postgres's deparser
prints them), prolly index trees, views and materialized views, functions
and procedures, enum, domain and composite types, sequences, and
extensions by name and version (their members aren't listed). Strings are
UTF-8 and names are qualified whatever the session's search path (it is
pinned while deparsing), so OIDs, relfilenumbers, creation order and
dropped columns never reach the hash. Data and prolly index trees are
shared with the working set, as they are canonical as written; at commit
(and before a merge commit) the canonicalizer's `prepare` rewrites the
rows of tables whose rows aren't canonical: rows from before a column was
dropped (the keying check rebuilds them) or added with a default (written
again with it), once per schema change (a META record notes the keying
the rows were last made canonical under). Objects not covered yet
(aggregates, policies, partitioned and foreign tables, other types) make a
commit fail, never a hash that might depend on history; heap tables are
not versioned and are left out. The engine gained `tree_root`; reclamation
walks canonical roots through the canonicalizer. Tests: `021_d1_canonical.pl`
(two databases reach the same content by different histories and get the
same root hash, which fails without the rewrite of rows from before an
added column; one changed value changes it; committing again keeps it;
views, functions and triggers count; an aggregate and a policy make a
commit fail). Found while running the requirements suite: a function
left by one test made every later commit in the database fail, which led
to covering functions, views and triggers. The requirements suite's
`201_history_clusters.pl` now passes (two clusters, different OIDs and
histories, enums and a dropped column, the same root hash), and so does
`vc_canonical`'s cross-database check. Not yet: a pinned deparse format
under the format stamp (question 35; today's deparser output is used
as it is), collation versions in the canonical schema (question 46),
portable codecs for `reg*` types (question 35).

**Status (2026-10-15): 4g.4 built (functions, views, connections).**
Doltgres's SQL interface (question 52), in a file of adapters onto the
refs and diff/merge interfaces: `dolt_commit`, `dolt_checkout`,
`dolt_branch`, `dolt_merge`, `dolt_reset`, `dolt_add` take Dolt's
arguments as `VARIADIC text` and return `text[]` as Doltgres's do;
`dolt_hashof`, `active_branch`; the `dolt_log`, `dolt_branches` and
`dolt_status` views; `dolt_diff(from, to, NULL::t)`, whose rows are the
table's row type as it was and is (Doltgres spreads them into `from_` and
`to_` columns; a row type is the Postgres way to return a table's columns
from one function for any table); `dolt.author` ("Name <email>"; the
session user when empty) for commit authors (question 56); and
`dbname/branch` connections (question 53: the part after the first `/`
when no database has the whole name; a branch that doesn't exist fails the
connection). There's no separate staging area: a commit records the whole
working set, so every change counts as staged (`dolt_add` has nothing to
do, `dolt_status` shows changes as staged, `dolt_reset` only does `--hard`).
`WORKING` is the branch's working set as last committed, so status and
diff don't see the current transaction's own writes. `dolt_commit` refuses
a commit whose root equals its parent's, as Dolt does, unless
`--allow-empty`. Interfaces: refs gained `branches`, `working_set` and
`reset` (a hard reset: the branch's head commit and working set become a
commit's, enqueued at commit under the branch's lock), and `resolve` takes
Dolt's ancestry steps (`HEAD~2`, `HEAD^2`); branch names can't start with
`-` or hold `~`, `^` or whitespace; diff takes one table or all, and the
ref `WORKING`; merge takes a message and `PROLLY_MERGE_NO_FF`, and
fast-forwards (a reset to the other head) when the current head is the
common ancestor. Tests: `022_d1_dolt_sql.pl` (authors, empty commits,
log order and ancestry, branches and connections to them, status, diffs
of keyed, keyless and large rows, fast-forward, merge commit and
`--no-ff` merges, hard resets through a crash, branch copy and delete).
Not yet: conflict tables (the next stage), `dolt_diff_<table>` and other per-table system tables (they
need a relation per table, which waits for versioned catalogs), schema
changes in `dolt_status`, remotes.

**Status (2026-10-15): 4g.4 built (merge conflicts).** Dolt's conflict
model (question 55), for rows. A merge applies every change that doesn't
conflict, keeps the current branch's row where both sides changed a key
differently, and leaves the merge in progress without a commit: the refs
keep it per branch (`'M'` entries: the merged head, the common ancestor,
and the tables with unresolved conflicts), so it is transactional and
crash-safe, and the conflicts themselves are found again from the three
commits whenever they're listed or resolved (no copy of them is stored).
`dolt_conflicts` lists tables and counts; `dolt_conflicts(NULL::t)` gives
each conflict's base, our and their rows (of the table's row type, as
Doltgres's `dolt_conflicts_<table>` columns) and how each side changed it;
`dolt_conflicts_resolve('--ours'|'--theirs', table | '.')` resolves a
table (taking theirs writes their rows through the executor, as the merge
does); `dolt_status` shows conflicted tables, unstaged; `dolt_commit` then
makes the merge commit, with both heads as parents; `dolt_merge('--abort')`
(a hard reset) ends it. As in Dolt, a transaction can't commit with
unresolved conflicts unless `dolt.allow_commit_conflicts` is on, so
outside a transaction block a merge with conflicts fails and changes
nothing. To allow that workflow (`BEGIN; dolt_merge; resolve; dolt_commit;
COMMIT`), a commit may now be made by a transaction that has written: the
commit records the working set the transaction will commit (later writes
in it are changes after the commit). Interfaces: merge returns its
conflict count and gained `conflicts` and `resolve` (with
`PROLLY_MERGE_FAIL_ON_CONFLICT`, kept by `prolly_merge()`, for the old
behavior); refs gained `merge_state` and `set_merge_state`, and refuse a
transaction's commit with unresolved conflicts. Tests: `023_d1_conflicts.pl`
(rollback outside a transaction block; listing; COMMIT and `dolt_commit`
refused; `--theirs` and `--ours`, the merge commit's parents and the btree
index; conflicts kept through a crash, a second merge refused, abort;
`prolly_merge()` still failing). Not yet: schema conflicts (they need
versioned catalogs), resolving single rows (Dolt deletes rows from
`dolt_conflicts_<table>`; here a table is resolved whole, after fixing its
rows by hand if need be), constraint violations (`dolt_constraint_violations`).

**Status (2026-10-15): 4g.5 begun: versioned databases are opt-in.**
`CREATE DATABASE ... VERSIONED` (question 54; otherwise the template's
setting, so a versioned template makes versioned databases) sets a new
`pg_database.datversioned`, and sessions see it as `MyDatabaseVersioned`.
In a versioned database, `prolly_d1` is the default table access method
(a dynamic default, below any setting for the server, database, role or
session), and a permanent table stored by any other table access method
can't be made: not by CREATE TABLE, CREATE TABLE AS, a materialized view,
SET ACCESS METHOD, or SET LOGGED on an unlogged heap table (question 25's
decision for user tables: every permanent table of a versioned database is
versioned). Temporary and unlogged tables are heap, as before. Other
databases are Postgres as it is; version control of the prolly tables
they hold still works, as before. Prolly tables and indexes ignore
tablespaces, with a NOTICE on CREATE TABLE, CREATE INDEX and SET
TABLESPACE (question 29's decision). pg_dump writes `VERSIONED = true`.
Tests: `024_versioned_database.pl`. The catalogs themselves are still heap
tables, shared by every branch: making them prolly tables is the rest of
4g.5 (below).

**Fix (2026-10-15): a relation's trees are found by its OID.** D1 found
a relation's trees by relfilenumber, which the catalogs (shared by every
branch until they're versioned) change on TRUNCATE, rewrites (ALTER TABLE
... TYPE, CLUSTER, VACUUM FULL, SET ACCESS METHOD) and SET TABLESPACE: a
TRUNCATE on one branch emptied the table on every other branch and in
every commit made before it. Trees are now found by relation OID, as the
root layout was meant to be (`09-versioned-catalogs.md`): TRUNCATE empties
the relation's trees on the current branch only; a rewrite builds the new
contents in a new relation and the engine's new `tree_swap` exchanges the
two relations' trees as the catalogs swap their storage
(`swap_relation_files`); SET TABLESPACE moves nothing; a prolly index build
starts from an empty tree; and the new `tree_drop` removes a dropped
relation's trees from the branch, which until now stayed reachable (and
unreclaimable) forever. TIDs stay unique to a table across its storage
(the row identity's new `counter_carry`), as other branches and commits
keep rows with theirs. Swaps and drops roll back with savepoints, and drops
carry through PREPARE. Tests: `025_d1_relation_trees.pl` (another branch,
its prolly index and a commit's rows through TRUNCATE, VACUUM FULL and a
type change; TIDs after them; savepoints; SET ACCESS METHOD both ways; a
crash; dropped tables' nodes reclaimed, prepared ones too). Still wrong
until catalogs are versioned: DDL on one branch applies to every branch's
rows (an index rebuilt with a new OID is empty on other branches; a type
change reinterprets their rows), and indexes of other access methods
(btree) are shared by every branch, so a TRUNCATE or rewrite on one branch
empties or rebuilds them for all.

**Fix (2026-10-15): VACUUM doesn't reuse TIDs a commit or branch may
hold.** One TID counter serves every branch of a table, and VACUUM on one
branch put the TIDs it forgot on the table's free stack: a branch made
before it, or a reset to a commit made before it, still had rows with
those TIDs, and its next inserts took them (two rows with one TID). The
refs now keep an epoch, counted up by every commit and every new branch
(roots that may share a working set's rows); a table's counter page pins
the numbers it had given out when it sees the epoch change, and VACUUM
puts only numbers above that on the free stack (it still removes the gone
entries on its branch). Conservative: numbers given out between a commit
and the next VACUUM aren't reused either. Found while fixing it: the refs'
changes at commit were sorted by `list_sort()`, which isn't stable, so of
several changes to one key (a merge's state set, then cleared) an earlier
one could win once a transaction made enough changes; they are ordered by
when they were made now (`023_d1_conflicts.pl` failed without it, once
commits also counted up the epoch). Tests: `026_d1_tid_reuse_branches.pl`
(a branch's and a reset's new rows keep distinct TIDs, which fails without
the pin; TIDs are reused without commits or branches).

**Status (2026-10-15): 4g.5 stage 1, catalog writes through the table
AM.** The six `CatalogTuple*` functions (`indexing.c`), through which all
of Postgres's catalog writes go, call the catalog's table access method
(`table_tuple_insert`, `table_multi_insert`, `simple_table_tuple_update`,
`simple_table_tuple_delete`) with the tuple in a slot, copying the TID the
access method gave it back into `t_self`; whether an update needs index
entries comes from the access method's `update_indexes` (heap's HOT
decision), not from the tuple's HOT flag, which the access method may set
on a copy. Heap catalogs take the same path, so every test exercises it;
behavior is unchanged (core's 239 tests pass). Not yet: the scans that
call `heap_getnext` directly (stage 1's other half).

**Status (2026-10-15): 4h begun: gates 1-3 run.** Postgres's own regress,
isolation and recovery suites ran with `prolly_d1` as the default table
access method (`specs/results/2026-10-15-gates-prolly_d1/`). They found
twelve bugs, fixed in this stack, each with its test: four crashes
(parallel btree builds, a WITHOUT OVERLAPS primary key, a savepoint
rollback in a fresh backend, a TID counter page left empty by a crash);
four hangs, two of them over wrong answers underneath (SSI missed
prepared writers, so `prepared_xacts` waited on a transaction that should
have failed; size estimates ignored spilled writes, so `join_hash` ran
nested loops for hours; `tuplelock-update` and
`tuplelock-upgrade-no-deadlock` waited where heap doesn't); three
gaps (backward scans, VACUUM's index cleanup, FOR KEY SHARE across no-key
updates); and a leak (SET TABLESPACE kept a prolly table's old storage). What
still differs is listed there: mostly heap-specific output (row order,
plans and costs, TIDs, sizes, TOAST, BRIN, multixacts, statistics), SSI at
table granularity (more serialization failures, never fewer), logical
decoding (deferred), and a short list to triage (EvalPlanQual and MERGE
isolation specs (the EvalPlanQual ones' errors fixed since, below; what's
left is row order and TIDs), `fk-snapshot`, `propagate-lock-delete`,
`insert-conflict-specconflict`, `056_standby_snapshot_export`). Open decisions:
types without binary I/O (stored as text, or refused as decision 15
says), unlogged prolly tables, and indexes of other access methods on
versioned tables (`09-versioned-catalogs.md` part 4). After the gates, the
S8 smoke run found a thirteenth bug (an index or bitmap scan feeding an
UPDATE read a table its first write had rebuilt in a new keying), fixed
too. The S and V smoke runs (one repetition, five-second windows, an
optimized build; `specs/results/2026-10-15-*-prolly_d1-smoke*/`, kept
out of git like all results) are D1's first numbers: every S workload
runs, at 0.01x-0.56x of heap's smoke figures (point lookups about 0.5x;
writes about 0.2x; range scans 0.04x; bulk load and large transactions
about 0.01x). V1-V3 run; V3 with background writes fails, as a merge
needs its branch to have no changes since its head commit (Dolt merges
into a dirty working set when the changes don't conflict: a decision);
V4 (clone, push) needs remotes, not built. Then a fourteenth: an update whose row's
newest version its own command wrote (a data-modifying WITH) got an
unexpected lock status; locking the end of an update chain now reports a
version this transaction wrote later as modified by it, as heap does.

**Exit criteria:** the S and V suites run against D1+K1, the requirements
suite (`src/test/prolly`) passes against it, and the correctness gates in
`06-benchmarking.md` are run, with their status recorded.

## 5. Measure the first engine

- Run the full B0 baseline (deferred from step 3) and both suites against
  D1+K1, on the same machine, in benchmark mode, with the same configuration.
- Collect profiles (`perf`) for the workloads where D1+K1 differs most from B0.
- Settle the chunk rule and how chunks sit in pages (questions 3 and 45).
  The 4g freeze doesn't bind either yet: the format stays provisional until
  every engine implementation has been evaluated (user, 2026-10-14). The
  plan (user, 2026-10-14): Dolt's chunking rules decide what a chunk is,
  and Postgres's page machinery decides where it lives.
  - **Why:** the starting chunker (`keyhash`) aims at about 31 items per
    leaf whatever their size. In 4e.1 the node counters showed what that
    costs: narrow items leave pages mostly empty. A 20,000-row scan of an
    `int` key plus a 60-byte text column reads 1,334 nodes: about 650 data
    leaves at about 3 KB each, and 655 row-identity-map leaves at about
    800 bytes each, in 8 KB pages. Filled by bytes to about 4 KB, it would
    read about 600.
  - **A chunk is canonical; a page is storage.** Boundaries count canonical
    bytes (encoded keys and values), never on-page bytes (line pointers,
    alignment, page headers, the cached hash), so a physical layout change
    can't change a hash. The minimum (about 512 bytes) and the maximum
    (a fixed constant, about 7.5 KB, that fits an 8 KB page after its
    overhead) are format constants, never derived from `BLCKSZ`. Prolly
    tables require `BLCKSZ` of at least 8192, so a canonical chunk always
    fits one block (question 45).
  - **Tree nodes: `keyhash_weibull`,** a second chunker implementation,
    shaped like Dolt's default `keySplitter`. The boundary hash covers the
    key alone, salted by level; it is separate from the content hash, fast
    and fully specified (xxh3, or the current FNV-1a with splitmix), since
    it is frozen into the format. The chance of a boundary rises with the
    canonical bytes since the last one, following a Weibull curve (K = 4).
    At a 4 KB scale a node reaches the maximum about once in 10^5, so the
    forced split is rare, and deterministic and the same on every build.
  - **Value bytes: a rolling hash.** Out-of-line values (4f) are split by
    a buzhash rolling hash over their bytes, as Dolt splits blobs; with
    them out of line, tree items stay small and the Weibull rule decides
    nearly every boundary (question 48).
  - **Pages hold several chunks: a packing node store,** a new
    implementation behind the node store interface. It appends a commit's
    new chunks to pages, never splitting a chunk across pages, and seals
    each page: written once, logged once as a new page, so checkpoints
    never force another full-page image, and checksums and torn-page
    protection work as for any page. A reference is (block, offset,
    length), which the extent type already allows; a chunk stays within
    one page, so items are still read in place from a pinned buffer.
    Readers of published chunks need only a pin. The cost is the unused
    tail of each commit's last page; if small commits make that matter, a
    per-backend tail page takes appends logged as small chunk records.
    Reclamation leaves holes in packed pages; the hash -> location map
    stays, so compaction moves live chunks to new pages and updates only
    the map, never their parents.
  - **Locks are never keyed on nodes.** Row locks (K1) and SSI predicate
    locks are keyed on rows and key ranges, so a boundary that moves when
    a value changes size can't cause false conflicts or miss real ones.
    That removes question 3's reason for key-only, item-count boundaries
    (`02-postgres-integration.md`, A4).
  - **Measure,** for `keyhash` and `keyhash_weibull` at 4, 5 and 6 KB
    scales, each with one chunk per page and with the packing store:
    - Node fill and page fill, per tree kind (data, row-identity map,
      prolly indexes, catalogs), from the node counters and a walk of
      page occupancy.
    - Nodes and pages read by scans, lookups and diffs.
    - Nodes written by a value update that keeps its size, by one that
      changes its size (byte-weighted boundaries can move; the trees
      should agree again within about a node), and by inserts and
      deletes.
    - Store size, WAL volume, and shared-buffer hit rates.
  - **Prove** that the rule is history-independent: a property test builds
    the same content through different orders of inserts and deletes, on
    builds with different `BLCKSZ` (8 and 16 KB), and gets the same root
    hash.
  - **Decide:** the rule, its scale and the store, recorded in questions 3
    and 45, with the chunker and node store interfaces keeping every
    implementation swappable.

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
