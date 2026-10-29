# Working set ⇄ prolly tree lifecycle

How a schema's Postgres working set and its prolly-tree history stay in step,
which artifacts exist, and when.  The aim: cheap version-control operations
for any commit inside an arbitrarily sized window.

## Pieces

| Piece | What it is | Lifetime |
|---|---|---|
| Working set | The schema's heap and indexes, mutable, plain SQL | Always |
| Change stream | One logical-decoding slot per versioned schema, read by one persistent streaming decoder | Always |
| Prolly tree | Commits, plus a *working root* per branch | Commits: per the window; working roots: always |
| Keyframes | Commit images (page images + page map) at selected commits | Local only, inside the window, never exported |
| Window | Incremental head coverage, sever into segments, export or discard | Always |

Only keyframes are optional.  Everything else is required.

## Lifecycle

1. **Writes.** SQL changes pages as usual.  Version control adds no work to
   individual writes; the standing costs are logical-level WAL, plus hint
   logging (data checksums or `wal_log_hints`) when keyframes are used.

2. **Checkpoint.**  The streaming decoder parses committed changes as they
   arrive and applies whole transactions to the branch's working root in
   batches, by row count or time (background checkpoints), confirming its
   position so the slot advances.  An on-demand checkpoint (commit, diff,
   status, checkout) is a *barrier*: a non-transactional logical message
   with a fresh token; when the stream reaches it, every transaction whose
   commit record precedes it has been applied.  Cost: O(rows since the last
   background apply).  No WAL is decoded twice.

3. **Dolt commit.**  No lock, and writers are never blocked.  The caller
   commits its own SQL transaction, then: barrier, commit chunk, return the
   hash.  "Synchronous" only means the caller waits for its commit's
   contents to reach the prolly tree.
   - The commit holds exactly the working root as of the barrier: every
     transaction whose commit record precedes the barrier in the WAL, each
     all or nothing.  Transactions committing later land in the working
     root afterwards, like uncommitted Dolt working-set changes, and the
     branch's working root carries on.
   - That cut follows WAL commit order: exactly what a replica replaying the
     same WAL shows.  In rare corner cases it can differ from what an MVCC
     snapshot taken at the same instant sees, because a commit record is
     written slightly before the transaction becomes visible to new
     snapshots.
   - Then the keyframe decision (below).  A keyframe is captured under an
     MVCC snapshot taken at the barrier, so it does not block writers
     either; if none is cut, commit does nothing physical.

4. **diff, status, log, merge base.**  Checkpoint, then prolly tree only:
   O(d log n) against any commit in the window.  Diff sees committed SQL
   state; a session's own uncommitted changes appear once it commits.

5. **Checkout, reset, branch switch** (move the working set to commit X).
   A planner picks the cheapest of:
   - **Row replay** (default): apply the prolly diff working root → X as DML,
     O(d × (1 + indexes)), under a replication origin so the change stream
     skips it.
   - **Keyframe + replay**: restore the nearest keyframe K, O(pages differing
     between the working set and K), then replay K → X.  For diffs that are
     large relative to the pages they touch: bulk changes, many indexes,
     distant targets.
   - **Full rebuild** from the prolly tree: for very large diffs or targets
     outside the window.

   Afterwards the working root is X's root and the slot is advanced.  The
   working set's physical base becomes K if a keyframe was used; otherwise
   it keeps its base plus what replay dirtied.

6. **Merge, cherry-pick, revert.**  Computed in the prolly tree, conflicts
   recorded there, result applied to the working set by row replay.

7. **Window.**  Commits and keyframes are held inside it.  Severing detaches
   commits into segments and releases their keyframes; segments are exported
   to a remote or archive, or discarded.

8. **Remotes.**  Prolly tree only: push, fetch, clone, attachment rules,
   boundaries and hints.

9. **Outside the window.**  Attach or deepen first, then rebuild from the
   prolly tree, or keyframe + replay if a keyframe is near.

10. **Crash recovery.**  The working set is durable through Postgres; the slot
    position and *working root = working set as of L* are durable; decoding
    replays from the slot.

## Keyframe policy

- **Default: none.**  Prolly tree + change stream covers commit, diff, merge,
  remotes, and most checkouts by row replay.
- **Keyframes where measurement says they pay:** at branch heads (the latest
  per branch: switching heads is the most common move), and along history
  each time *R* rows have changed since the last keyframe, where *R* is the
  measured point at which restoring pages beats replaying rows.  If replay
  always wins once restore's constant factor is fixed, *R* = ∞ and there are
  no keyframes.
- **Restoring a keyframe is exact only if** every page change after its base
  point reaches the WAL and is read: hint changes logged, a checkpoint at the
  base point, and the WAL flushed before the changed pages are read
  (`bench/lp_dead_repro.sql` shows each failure).
- **Longer term:** copy-on-write page storage (next section) makes every
  commit's page-map root a free keyframe, once settled.

## Concurrent branches: copy-on-write working sets

**The limitation.**  As built, a versioned schema has one physical working
set, and it holds exactly one branch, the checked-out one.  Every branch
has its own logical working set (its working root in the prolly tree), and
everything at the version-control level scales to a million branches, but
two sessions cannot run SQL against two different branches at once: one
would have to switch the working set (a replay, 100–500 ms measured),
which moves it for everyone.  Concurrent writers on *one* branch are fine.

**The target design: one copy-on-write working set per branch.**  Each
branch's working set is its own page map over a shared, content-addressed
page store.  Pages are shared until a branch writes one; the write puts a
new page in that branch's map only, so branches stay isolated.

| Operation | One physical working set (built) | Copy-on-write working sets (target) |
|---|---|---|
| Branch create | O(1) in the tree | O(1): copy the branch's page-map roots |
| A session on a branch / checkout | Replay the difference; moves the working set for everyone | O(1): connect to that branch's own working set; sessions on different branches run concurrently |
| Reset to commit X in the window | Replay, O(changed rows × indexes) | O(1) if X kept a settled page-map root; otherwise the nearest settled root plus replay |
| diff WORKING X | Checkpoint, then prolly diff | Unchanged: the branch's own change stream feeds its working root |
| Rebase, merge, cherry-pick, revert | Computed in the tree, applied by row replay | Unchanged, replayed into that branch's working set only |
| Commit | Barrier, then the prolly commit | Same, plus freezing the branch's page-map root in O(1): its keyframe, once settled |
| Branch delete | Drop the working root | Drop the branch's database; frees only the pages no other root shares |

The window's guarantees are kept and two get stronger: switching becomes
O(1) whatever the change size, and so does reset to any commit whose
settled root is retained.

**What has to hold:**

1. **Commit roots must be settled before reset may use them.**  A frozen
   page map can hold tuples from transactions in progress at the commit;
   if they commit later, resetting to that root would show their rows.  A
   commit's root is settled by removing what its MVCC snapshot cannot see
   and freezing the rest: O(changed pages) per commit, not O(data).  An
   unsettled root is never a reset target; reset uses the nearest settled
   root plus replay, which stays correct.
2. **Each branch has its own identity inside Postgres.**  Shared buffers,
   catalogs and WAL name pages by (database, relation file, block), so two
   branches cannot share those names.  One database per branch, created by
   copying page-map roots, gives that: catalogs (and so DDL) per branch,
   sessions connect to their branch's database, buffers and WAL unchanged.
   Branches share the cluster's transaction ids and commit log, so pages
   shared between branches stay valid.
3. **A storage manager backed by the page store** resolves (relation,
   fork, block) through the branch's page map and writes copy-on-write;
   recovery replays WAL into the page store.  Core Postgres has no
   pluggable storage manager API; the fork adds one.

**What it costs.**  Every page write also updates the branch's page map
(O(log n)); that overhead against plain Postgres is the first thing to
measure.  Each settled root kept in the window costs the pages changed
since the previous one, the same as a keyframe, so the retention policy
still applies; without a root, reset falls back to replay.  It is the
largest component so far: storage manager, buffer manager and recovery.

## Mode: dolt tx commit

A user-configured mode, off by default, enabled per versioned schema: every
SQL transaction becomes its own Dolt commit.  The aim is clean history:
each commit holds exactly one transaction's changes, so its diff against
its parent is exactly what that transaction did.

**No temporary branch per transaction is needed.**  The branch-and-merge
form (a branch per SQL transaction, merged into the default branch at
COMMIT) gives the same result, but Postgres already does both halves:
MVCC isolates the transaction, and at COMMIT its changes are already part
of the default branch's working set, write conflicts settled by row locks
or a serialization failure, so such a merge could never conflict.  The
change stream delivers committed transactions one at a time in commit
order, and replaying them in that order reproduces the database (as
logical replication does).  So with the mode on, the decoder writes one
commit per decoded transaction: its parent is the previous transaction's
commit, its diff is that transaction's net row changes, and history is
linear.  Copy-on-write is not needed for any of it.

**Asynchronous.**  SQL COMMIT returns as usual; the Dolt commit appears
once the decoder reaches it (milliseconds), and its hash can be found by
transaction id.  COMMIT latency is unchanged.  (A synchronous form exists
if ever wanted: the decoder as a synchronous standby, so COMMIT waits for
it; decoder throughput would then cap the write rate.)

**Commit contents and metadata.**  Each commit records the transaction id
and commit timestamp from the stream; a session can give it a message
with a transactional logical message inside the transaction, so the
message commits or aborts with it; the transaction's DDL joins it once DDL
is captured.  An explicit Dolt commit, with the mode on, becomes a named
marker (a tag) on the per-transaction history, or a squash commit for
coarser history; diffs between markers are prolly diffs as usual.

**What changes in the model:**

| | Mode off (explicit commits) | Mode on (dolt tx commit) |
|---|---|---|
| Who makes commits | `dolt_commit`, through a barrier | The decoder, at each decoded transaction |
| Working root | Ahead of the head between commits | Equal to the head, apart from the undecoded tail |
| Tree writes | Batched: many transactions per tree edit | One tree edit per transaction |
| Storage per change | Path copies shared by a batch | ~tree height × node size per touched leaf; a 1-row transaction ~3–4 nodes (12–16 KB at 4 KB chunks) |
| Throughput limit | Not the decoder | The decoder's commits per second: ~100–250 µs per commit measured at the engine level, ~4–10k transactions/s per decoder |
| Window size | A count of Dolt commits | By time or bytes: a count would mean N transactions |

**The window's guarantees are unchanged.**  Diff, checkout, reset, merge,
severing and remotes cost the same per change, and every per-transaction
commit in the window can be diffed against or checked out.  What grows is
the number of commits and the storage per unit of change, which the window
bounds; that is why the window should be sized by time or bytes in this
mode (see the time-based window: corrected commit times recorded in each
commit, so the edge is found without reading past it).

**To measure before relying on it:** decoder throughput and lag at high
transaction rates; storage per transaction for small transactions (smaller
chunks for heavily written tables cut it, at the cost of taller trees);
the latency spike from a very large transaction, which arrives at commit
as one unit.

## Conclusions of the prototype

What it proved, against a real Postgres where it matters:

- **The history window works at scale.**  Severing moves holds only;
  segments export to an archive or remote, are discarded with exact frees,
  or come back verified; remotes respect both sides' windows and refuse
  histories that do not attach.  Per-operation costs stay flat from 1k to
  1M branches (`bench/branchbench`).
- **The lifecycle needs no artifact.**  Prolly tree plus one streaming
  decoder: commit ~71–74 ms whether the table is 1.8 GB or 17 GB; diff and
  checkout cost what changed, not the table's size; commits capture
  exactly the state as of their barrier under concurrent writers.
- **Inside the window is what makes it cheap.**  Outside it, diff costs
  ~6–9× more and checkout ~2× (bringing the old version back from the
  archive); a full rebuild costs ~100×.
- **Keyframes as built lose to row replay** on random workloads (~150 µs
  per page restored vs ~8 µs per row replayed), so the default is none.
  Restoring pages exactly also needs logged hints, a checkpoint at the base
  point, and a flushed WAL (`bench/lp_dead_repro.sql`).

- **Severed history really leaves the disk.**  Immutable chunk files and
  compaction keep disk at the live size while history grows (1.8 GB
  without compaction vs 196 MB with it, over 200 commits).

What it left open:

- **Concurrent SQL on different branches** needs a working set per branch;
  the target is copy-on-write working sets (above), the production storage
  layer rather than a prototype.  A cheaper interim (a database per active
  branch, cloned with `file_copy_method = clone` on a copy-on-write
  filesystem) would test the lifecycle's fit without it.
- **O(1) reset** through settled commit roots, which comes with
  copy-on-write.
- **Generic tables and DDL** (row codec for any table, DDL capture), the
  verbs not built (rebase, revert, cross-branch reads, stored conflicts),
  the time-based window, and **dolt tx commit** measured at high
  transaction rates.
- **BranchBench**, which needs the generic codec and DDL, and per-branch
  working sets for its multi-agent scenarios.

## The chunk store on disk

`dirstore` keeps every chunk in a directory of **immutable chunk files**
plus a `MANIFEST`.

| File | Contents | How it changes |
|---|---|---|
| `c-<id>.chunks` | records (u32 length, 20-byte hash, the chunk's bytes), then an index sorted by hash (hash, offset, length), then a footer (magic, format, count, index offset, hash of the index) | written whole to a temporary name, fsynced, renamed into place; never modified |
| `MANIFEST` | format, next file id, the live chunk files, the durable root set (hash, hold count), the repo-state root, and a hash of it all | replaced whole by rename |

- **Writes** collect in memory and a flush writes them as a new file, then a
  new manifest.  Work not yet flushed is lost in a crash; in the real
  system the WAL covers it.
- **Opening** loads each file's index, deletes files the manifest does not
  name (crash leftovers), and rebuilds reference counts by walking from the
  roots; anything unreachable is dead.  That walk is O(live chunks): fine
  here, while at terabyte scale the counts would be persisted.
- **Repo state** (branches, commit locations, segments, hints, image holds,
  window tracking) is itself a chunk the manifest names (`vc_repo_save`,
  `vc_repo_open`).
- **Garbage collection is compaction:** the live chunks of files with
  enough dead space, and of small files, are copied into new files, a
  manifest without the old files is installed, then the old files are
  deleted.  Only then does disk space come back.
- **Proven by tests** (`test/test_dirstore.c`): round trips through
  reopening; chunk files unchanged byte for byte (and in mtime) across
  puts, flushes, frees and compaction; corruption detected; repo state
  saved and reopened; a crash at every point of flush and compaction
  recovered from, with no stray files left.

**Severing gives disk back.**  `bench/diskbench`: 1M rows, 4 KB chunks, 200
commits of 2,000 random updates, a 20-commit window, history severed
behind it every commit.

| Commits | Disk, no compaction | Disk, compaction every 20 (discard) | Disk, compaction every 20 (export) | Live bytes |
|---|---|---|---|---|
| 20 | 206 MB | 206 MB | 206 MB | 195 MB |
| 60 | 565 MB | 196 MB | 196 MB | 194 MB |
| 100 | 924 MB | 196 MB | 196 MB | 194 MB |
| 200 | 1,824 MB | 196 MB | 196 MB | 194 MB |

Without compaction disk grows ~9 MB per commit, keeping every byte ever
written; with it, disk stays at the live size (~1% overhead) however long
history gets.  Exporting behaves the same locally, with the severed
history (1.6 GB) in the archive.  Compaction copied 1.18 bytes per byte it
gave back (1.5 s in all): random updates spread dead space through every
file; grouping chunks by age would lower that.

**Compared with Dolt's generational GC.**  Dolt (`go/store/nbs/generational_chunk_store.go`,
`go/store/types/value_store.go`) keeps two chunk stores, an old and a new
generation, each of immutable table files plus a manifest (new writes go
through an append-only journal).  A collection traces from the roots:
chunks reachable from branch, remote and internal refs (committed
history) are copied into the old generation, chunks reachable only from
other roots (working sets) into fresh new-generation files, and the rest
of the new generation is dropped.  By default the old generation is never
collected (chunks already in it are skipped), so committed history only
accumulates; `dolt gc --full` traces and rewrites it too, at O(all live
history).  The bet: garbage is recent churn and history is permanent.

| | Dolt (generational, tracing) | Here (reference counts, compaction) |
|---|---|---|
| Finding garbage | Traces from the roots at each collection | Exact reference counts: garbage is known when its last hold goes |
| Where garbage is expected | Recent churn; history is permanent | Committed history leaving the window, which Dolt's default collection never looks at |
| Cost of a collection | O(live data in the generation collected); the old generation only with `--full`, O(all history) | O(live bytes in the files rewritten); no tracing, except rebuilding counts on open |
| Reclaiming severed history | Only `--full` | Ordinary compaction |
| Files | Immutable files, manifest swap | The same |
| New writes | Append-only journal | Memory until a flush (the WAL covers them in the real system) |

Dolt's default collection is cheap because it ignores history; the
window makes dropping or offloading history routine, which under Dolt's
scheme would need a full collection each time.

Worth taking from it:

- **Placement by age.**  Compaction here copied 1.18 bytes per byte given
  back because random updates scatter dead space through every file.
  Grouping chunks by age (say, the commit generation that first wrote
  them) would make history leaving the window tend to free whole files,
  with little or no copying: generational GC's real insight, that things
  which die together should live together.  The window gives the
  boundary.
- **Archive compression.**  Dolt's archive format compresses old data with
  shared dictionaries; it suits exported and archived segments.

Neither changes the design; both are for the real implementation.

## Rebuilding Postgres from the prolly tree

The question: does a commit hold everything Postgres needs to work, with
every kind of index?  Indexes are not stored in the tree.  Postgres builds
them from rows plus their definitions, so a commit must reproduce two
things exactly: every row, and the whole schema.  Then every index Postgres
builds from them must behave as the original did.

**What a commit holds** (the logical working set, `bench/rebuild.c`, behind
a `WorkingSetOps` so that another codec can replace it):

| Root value entry | Schema chunk | Data |
|---|---|---|
| `~pre` | pg_dump's pre-data section of the schema: types, domains, collations, functions, tables, sequences, partitions, identity, defaults, validated checks | none |
| `~post` | pg_dump's post-data section: indexes, PK/unique/exclusion/FK constraints, NOT VALID checks, triggers, policies, CLUSTER ON, statistics objects, the REFRESH of populated materialized views | none |
| `~seq` | none | sequence → (last_value, is_called) |
| each table holding rows (leaf partitions included) | the codec layout: key column count, column list | COPY text rows keyed by the primary key's fields; without a primary key, the whole row → its count |

Rows are stored as COPY text under fixed session settings (ISO dates, UTC,
shortest exact floats, hex bytea), so they are portable across Postgres
versions and keep NULL distinct from the string `\N`.  Generated columns
(stored and virtual) are not stored, because Postgres computes them again.
Extensions live outside the schema (the unit of version control), so every
database has them installed before a rebuild.

**Rebuild** into an empty database (extensions only): run `~pre`, COPY every
table, restore sequence positions, run `~post` (indexes are built here,
constraints validated, materialized views refreshed), then VACUUM ANALYZE.

**The test** (`make rebuild-check`, against the running server):

1. Four commits are made from the fixture in `bench/rebuild_fixture/`: a
   load, then row changes, then DDL changes, then rows again.  Each is
   captured into a repo in a durable `dirstore`.
2. The store is closed and reopened, and every commit in the log is rebuilt
   from the store alone.
3. Each rebuild must:
   - **capture back to the same root value.**  Root values are history
     independent, so this proves the schema dump and every row of every
     table are identical.
   - **pass amcheck:** `verify_heapam` with TOAST on every heap, and
     `bt_index_parent_check` (heapallindexed, rootdescend, checkunique) on
     every B-tree.  `gin_index_check` runs on every GIN index.
   - **answer the checks** in `checks.txt` as the source answered at that
     commit:
     - 63 queries, each answered through a named index.  The table's other
       indexes are hidden from the planner inside a rolled-back
       transaction, and the plan must use the named index.  The source
       answers with index scans off, so the index's answer is compared with
       the heap's.
     - 9 queries on sequences, views, the materialized view, generated
       columns and TOASTed values.
     - 17 statements each constraint must reject with its SQLSTATE: unique,
       primary key, exclusion, FK, check, NOT VALID check, domain, partition
       bound, not-null, identity.
4. Across the rebuilds, every index amcheck cannot verify (hash, GiST,
   SP-GiST, BRIN, bloom) must answer at least one query, and every query
   must return rows at some commit.
5. Four deliberate faults must each be caught (`--break`):
   - a lost row;
   - unrestored sequences;
   - matviews left unpopulated;
   - planning with every index visible.

**Result:** all four commits rebuild to the same root value, and every check
passes.  A rebuild takes about 5 s on the fixture: 49–51k rows in 18–19
tables, a 55 MB store.

| Covered in the fixture | |
|---|---|
| Index methods (first commit) | 26 btree, 13 GiST, 11 GIN, 8 BRIN, 6 SP-GiST, 3 hash, 1 bloom; 8 partitioned parent indexes |
| B-tree | composite primary keys, DESC NULLS FIRST, INCLUDE (covering), partial, expression (through a schema function), `text_pattern_ops`, composite-type field, fillfactor and deduplicate_items off, on a nondeterministic ICU collation (NULLS NOT DISTINCT), deferrable, on a matview |
| GIN | `jsonb_ops`, `jsonb_path_ops`, arrays, tsvector, `pg_trgm`, hstore, btree_gin over int8 and an enum, partitioned |
| GiST | point, box, polygon, circle, range, multirange, tsvector, `pg_trgm`, cube, inet, btree_gist multicolumn, two exclusion constraints, KNN ordering |
| SP-GiST | quad and kd point, text radix, inet, range, box |
| BRIN | minmax, minmax-multi, bloom, inclusion (inet and box), autosummarize, on every partition |
| Types | every built-in scalar, NaN, ±Infinity, −0, infinite dates, timestamps and intervals, BC dates, empty and multi-dimensional arrays, arrays with non-1 lower bounds and NULLs, enums (with a value inserted between others), composite, domain, ranges, multiranges, geometric, text search, bit strings, money, pg_lsn, json with duplicate keys, hstore, cube, ICU collations, tabs, newlines, backslashes, `\N` and empty strings, 128–260 KB values (compressible and not, EXTERNAL storage) |
| Tables | no primary key with duplicate and all-NULL rows, identity, stored and virtual generated columns, fast defaults (`ADD COLUMN … DEFAULT`), dropped and retyped columns, two-level range/hash partitions with a default partition, detach and attach, inheritance, unlogged, renamed, RLS, replica identity, comments, column statistics targets |
| Sequences | serial, identity (custom start and increment), cycling (wrapped), never called |

**Findings:**

- **Materialized views:**
  - `pg_dump --schema-only` drops the REFRESH of populated materialized
    views, so a rebuild left them empty (error 55000 on read).  Post-data is
    now dumped with data on; the section holds no rows.  `--break matview`
    keeps the regression covered.
  - Like any dump and restore, a rebuild refreshes a materialized view from
    the current rows, so a source view left stale between refreshes comes
    back fresh.  The view's contents are not versioned, only whether it is
    populated.
- **Indexes need nothing of their own.**  Every index, of every method,
  rebuilt from rows plus its definition and gave the heap's answers.  This
  holds at every commit, including after enum values were inserted between
  others and after column drops and retypes.
- **What the rows alone do not keep**, and Postgres does not need:
  - physical layout: the heap is reloaded in key order, so ctids, page
    contents, fill and correlation differ (BRIN stays correct, but its
    pruning depends on that order);
  - planner statistics, which are regenerated by ANALYZE;
  - OIDs and relfilenodes.
  None of these change any answer.
- **Large values make large chunks.**  A row is one item, so a 260 KB value
  is a 262 KB chunk, against the 16 KB target.  This is correct, but it
  defeats structural sharing for big values: changing a byte rewrites the
  whole chunk.  Dolt chunks large values into their own trees; the real
  codec should too.
- **Not covered:** large objects (not schema scoped); roles, grants to
  other roles and tablespaces (cluster objects); extension versions;
  publications, subscriptions, event triggers, foreign tables; `xml`
  (this build has no libxml).  Indexes on virtual generated columns are
  not supported by Postgres itself.

### Through the replication stream: DDL in the commit stream

The rebuild above captures each commit fresh.  The lifecycle instead
commits from the streaming decoder, so two problems had to be solved:
- **DDL.** Logical decoding carries rows but not DDL, so a schema change
  has to land at the right place among the row changes.
- **Unchanged TOAST.** An UPDATE that leaves a TOASTed value alone does not
  send that value.

`bench/rebuild --stream` drives the same fixture through a pgoutput stream.
At every commit its root value must equal a fresh capture's, and then every
commit is rebuilt and checked as above.

How it works (`bench/rebuild_stream.sql`, `bench/pgoutput.c`):

- **DDL travels in the stream as logical messages.**  An event trigger at
  `ddl_command_end` writes, inside the DDL's own transaction:
  - **`prolly.layout`:** every table's codec layout, so rows after the
    statement are read with the new layout and rows before it with the old
    one;
  - **`prolly.rows`:** the whole contents of each table whose row text the
    statement may have changed with no decoded changes of its own.  This
    covers ALTER TABLE (rewrites, fast defaults, dropped and retyped
    columns), CREATE TABLE AS and ALTER TYPE, and extends to partitions and
    inheritance children.  The decoder replaces the table with those rows.
  
  The layout query is the one the capture uses, so the two agree byte for
  byte.  Aborted transactions never reach the stream, and their DDL with
  them.
- **Definitions are dumped at the barrier.**  DDL takes a shared advisory
  lock at `ddl_command_start`, held to the end of its transaction.  A Dolt
  commit then:
  1. takes the lock exclusively;
  2. exports a snapshot;
  3. writes the barrier (non-transactional and flushed);
  4. releases the lock.
  
  The stream is applied up to the barrier.  The definitions
  (`pg_dump --snapshot`), sequence positions and unlogged tables, which the
  WAL does not carry, are read at the snapshot.  DDL waits only for that
  instant, and row writers never wait.
- **Unchanged TOAST values** are taken from the row's current value in the
  tree: pending edits first, then the tree.
- **Keyless tables** are given `REPLICA IDENTITY FULL` by the trigger,
  since their rows are keyed by the whole old row; without it Postgres
  refuses their UPDATEs and DELETEs once a publication covers them.  This
  shows in the schema dump.

**Result:** all four commits stream to the same root value as a fresh
capture, and every commit rebuilds and passes every check.
- **Exercised:** DDL and DML in one transaction, a rolled-back DDL
  transaction, TRUNCATE, CREATE TABLE AS, column renames, dropped and
  retyped columns, fast defaults, an enum value added mid-list, partition
  detach and attach, rows moving between partitions, primary-key updates,
  cascades, and unchanged TOAST values (50 filled from the tree).
- **Size:** about 112k streamed changes, 106 layout messages, 19 rows
  messages (101k rows re-sent).
- **Time:** the stream reaches the barrier in 0.07–0.71 s per commit, and
  the definitions take 0.09 s.

**Findings** (each kept by a `--stream --break` case):

- **Casts to text are not output functions.**  `bool::text` is `false`
  where the output function gives `f`, `char(n)::text` drops the padding,
  and `inet::text` adds `/32`.  COPY and the stream use output functions,
  so the trigger formats rows with `format('%s', …)` (`--break cast`).
- **`x IS NULL` is true for a composite value whose fields are all NULL**,
  which is not a NULL value.  The trigger uses `num_nulls(x) > 0`
  (`--break isnull`; the fixture now has such a row).
- **The barrier must be flushed.**  Logical decoding sends only flushed
  WAL, so an unflushed non-transactional message waited 1.6–20 s, until
  something else flushed the WAL.  This is the same lesson as the forced
  flush before reading changed blocks (`--break noflush`, caught by a 5 s
  deadline).
- **Rewriting DDL costs O(table) in the stream.**  The rows are re-sent
  whole, including for fast defaults and enum additions that Postgres does
  in O(1).  This is correct, but the real implementation should transform
  rows in the decoder where it can (add or drop a column, rename) and
  re-send only for real type changes (`--break noreemit` shows the rows
  are needed).
- **Not covered yet:**
  - the barrier protocol under concurrent DDL and row writers (it is
    implemented, but the fixture is sequential);
  - ALTER TYPE on composite types nested in other types or in arrays;
  - DDL from extensions;
  - `publish_via_partition_root`.

## Evidence so far

From `bench/winbench` against a real server (2,000 random row changes per
commit, two indexes) and `bench/branchbench` at the engine level.

| Measure | Result |
|---|---|
| Keyframe checkout, one commit away | 4,193 / 4,355 / 4,471 pages at 196 MB / 1.7 GB / 17 GB: follows the change, not the size |
| Keyframe restore cost (harness, via libpq, full-page WAL) | ~150 µs per page |
| Row replay, ~9,000 rows | 114 / 153 / 221 ms at the same sizes: currently ~12× cheaper than restore |
| Branches, 1k → 1M | create 2–8 µs, commit ~108 µs, image ~130 µs, sever 0.2 µs: flat |
| Switch between random heads | grows with the heads' distance, not with branch count |
| Correctness | every checkout verified row by row against the prolly tree, with amcheck and index-driven counts |

### Step 1: the lifecycle without keyframes

`bench/winbench --lifecycle` on 10M rows (1.8 GB), 2,000 random row changes
per commit, four branches; medians, every checkout verified row by row.

| Operation | Work | Time |
|---|---|---|
| Dolt commit | 2,000-row tail | 123 ms (checkpoint 114 ms) |
| diff WORKING X | 2,000-row tail, ~36k rows differ | 220 ms (checkpoint 152 ms, prolly diff 69 ms) |
| Branch switch | ~16k rows replayed | 233 ms (replay 166 ms) |
| Reset to a commit in the window | ~8k rows replayed | 162 ms (replay 70 ms) |
| Background checkpoints | 34 during the run | mean 61 ms, max 131 ms |

At 10× the data (100M rows, 17 GB, past the 16 GB buffer pool), same
workload, every checkout verified:

| Operation | 10M rows (1.8 GB) | 100M rows (17 GB) | Growth for 10× data |
|---|---|---|---|
| Dolt commit | 123 ms | 180 ms | 1.5× |
| diff WORKING X (~36k rows) | 220 ms (prolly diff 69 ms) | 298 ms (prolly diff 123 ms) | 1.4× (diff 1.8×) |
| Branch switch (~16k rows) | 233 ms (8.1 µs/row) | 502 ms (10.6 µs/row) | 2.2× |
| Reset (~8k rows) | 162 ms (8.6 µs/row) | 212 ms (11.4 µs/row) | 1.3× |
| Background checkpoint | mean 61 ms | mean 136 ms | 2.2× |

Costs follow the change, with sublinear growth from the taller trees and
from the table no longer fitting in the buffer pool (random row replay
starts to miss), not O(data).

With the persistent streaming decoder in place of the SQL function (same
workload; every checkout verified):

| | Commit | diff WORKING X | Branch switch | Reset |
|---|---|---|---|---|
| 10M, SQL decoding | 123 ms | 220 ms | 233 ms | 162 ms |
| 10M, stream | 71 ms | 138 ms | 171 ms | 75 ms |
| 100M, SQL decoding | 180 ms | 298 ms | 502 ms | 212 ms |
| 100M, stream | 74 ms | 207 ms | 431 ms | 99 ms |

Commit no longer grows with the data.  Background applies absorb most of
the tail (700–800 rows left for the commit's barrier instead of 2,000), and
an empty checkpoint costs 1–2 ms.  What still grows is the prolly diff
(taller tree) and replay once the table outgrows the buffer pool.

**Commits under concurrent writers.**  `winbench --lifecycle --concurrent`
runs a writer committing 50-row transactions nonstop while 40 Dolt commits
are taken, holding each commit back 150 ms after its barrier so later
transactions land in the working root first.  Every commit held exactly a
prefix of the writer's transactions: whole transactions only, everything
committed before the commit began, nothing from after its barrier
(8,248 writer transactions).  Committing the working root at lock time,
instead of the root as of the barrier (`vc_commit_root`), fails the last
check.

**Inside vs outside the window.**  `winbench --lifecycle --outside` on 10M
rows, 2,000 row changes per commit, 45 commits; the same targets with a
10-commit window (severed history exported to an archive store, a local
file here) and with a 60-commit window (everything inside); every checkout
verified.

| Commits back | diff, inside | diff, outside | checkout, inside | checkout, outside | Brought back from the archive |
|---|---|---|---|---|---|
| 5 | 28 ms | (inside) | 162 ms | (inside) | — |
| 15 | 63 ms | 555 ms | 343 ms | 835 ms | 11.6k chunks, 64 MB, 492 ms |
| 20 | 78 ms | 484 ms | 352 ms | 788 ms | 10.0k chunks, 54 MB, 406 ms |
| 30 | 109 ms | 820 ms | 530 ms | 1,196 ms | 17.8k chunks, 98 MB, 711 ms |
| 40 | 137 ms | 823 ms | 644 ms | 1,298 ms | 17.8k chunks, 98 MB, 693 ms |
| Full rebuild of the table from the tree | | | | 30.9 s | |

- Outside the window the only extra is bringing the target back from the
  archive: the chunks that version does not share with what is still
  local.  Diff and replay then cost what they do inside.
- That makes diff ~6–9× slower and checkout ~2× (replay is a larger share
  of a checkout).  The cost follows how far the old version has drifted
  from local history, not the table's size, and bringing one old commit
  back makes nearby ones cheaper (they share chunks).
- A full rebuild, what is left without a diff path, is 40–100× the
  inside-the-window cost.
- With a remote archive, only the bring-back part changes: it becomes
  bandwidth and round trips, ~100 MB per bring-back at these distances.

- Replay costs ~8 µs per row against ~150 µs per page for keyframe restore
  as built (~2 pages per row here): ~30× cheaper per change on this
  workload.
- Checkpoints dominate.  The same 2,000-row tail took ~45 ms at 200k rows
  and ~114 ms at 10M: partly the taller tree, mostly the SQL decoding
  function re-reading WAL from the slot's restart point on every call.  The
  persistent streaming decoder (below) removed it.

## Next

The prototype phase is concluded (see *Conclusions of the prototype*).  In
the real implementation, in order:

1. The lifecycle with the streaming decoder, a generic row codec and DDL
   capture (`bench/rebuild.c` is the reference: the logical working set,
   and DDL carried in the stream by an event trigger, with large values in
   their own trees and rewrites transformed in the decoder); explicit
   commits as of the barrier; the time-based window.
2. The **dolt tx commit** mode, asynchronous, measured at high transaction
   rates.
3. Copy-on-write working sets (a page-store storage manager, a database
   per branch, settled commit roots), for concurrent branches and O(1)
   reset.
4. BranchBench against it.
