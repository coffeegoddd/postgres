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

The keyframe planner (`kfplan.h`) decides, measured in *Keyframes:
replay or restore* below:
- **checkout:** restore the target's keyframe, or the nearest imaged
  ancestor's and then replay the rest, when that is priced lower than
  replaying the rows;
- **capture:** keep a keyframe at a commit when restoring the change since
  the last one, plus capturing it, beats replaying it;
- **costs:** each path's costs are refitted from every move the planner
  observes, starting from the costs measured for the server's
  wal_compression.
- **Exactness:** restoring a keyframe is exact only if every page change
  after its base point reaches the WAL and is read.  That needs logged
  hints, a checkpoint at the base point, and the WAL flushed before the
  changed pages are read (`bench/lp_dead_repro.sql` shows each failure).
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
- **Keyframes pay for large or dense changes** (superseded: the earlier
  "~150 µs per page, so none" was pglz compression and one WAL record per
  page; see *Keyframes: replay or restore*).  Restoring pages exactly also
  needs logged hints, a checkpoint at the base point, and a flushed WAL
  (`bench/lp_dead_repro.sql`).

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

**What a commit holds** (the logical working set, `bench/ws_logical.c` and
`bench/ws_codec.c`, behind a `WorkingSetOps` so that another codec can
replace it):

| Root value entry | Schema chunk | Data |
|---|---|---|
| `~obj` | none | every object of the schema as pg_dump writes it, one entry each: (section, type, name) → its DDL.  Pre-data: types, domains, collations, functions, tables, sequences, partitions, identity, defaults, validated checks.  Post-data: indexes, constraints, NOT VALID checks, triggers, policies, CLUSTER ON, statistics objects, the REFRESH of populated materialized views |
| `~seq` | none | sequence → (last_value, is_called) |
| each table holding rows (leaf partitions included) | its layout: the primary key and every column by tag (name, type, default, stored or generated) | rows keyed by the primary key's values; the value holds the other non-NULL columns as (tag, bytes) cells; without a primary key, the row's cells → its count |

The first version kept the schema as two pg_dump sections and rows as
positional COPY text.  Both were replaced so that diff and merge can work on
the schema (see *Structured schema* below).

Values are each type's output text under fixed session settings (ISO dates,
UTC, shortest exact floats, hex bytea), so they are portable across
Postgres versions.  NULLs are absent cells, so they cost nothing and stay
distinct from the string `\N`.  Generated columns (stored and virtual) are
not stored, because Postgres computes them again.  Extensions live outside
the schema (the unit of version control), so every database has them
installed before a rebuild.

**Rebuild** into an empty database (extensions and the tag table only):
1. run the pre-data objects, ordered by type, retrying any whose
   dependencies aren't there yet until no more succeed;
2. write the commit's column tags;
3. COPY every table;
4. restore sequence positions;
5. run the post-data objects the same way (indexes are built here,
   constraints validated, materialized views refreshed);
6. VACUUM ANALYZE.

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

### Structured schema: column tags, diff and merge

A schema stored as one pg_dump text can be rebuilt, but it can't be
merged: any DDL on two branches conflicts.  And positional row text can't
be read across a schema change.  Dolt versions the schema as structured
data instead:
- every column has a stable numeric **tag**, which survives renames;
- merges, diffs and foreign keys match columns by tag;
- indexes are stored as prolly trees of their own.

Dolt still rewrites every row for `ADD COLUMN … DEFAULT`, `DROP COLUMN`, a
reorder, a primary-key change, and a type change the stored bytes can't
absorb (`ShouldRewriteTable`); only ADD COLUMN of a nullable column without
a default is metadata only.  The prototype now does the same on Postgres's
terms.

**How:**
- **Objects:** the schema is a map of objects, one per pg_dump entry,
  keyed (section, type, name) → DDL.
- **Tags:** every column has a tag, kept in a `prolly.tags` table in each
  database.
  - A tag is given when the column first appears, from the table name,
    column name and type, so two branches adding the same column agree on
    it (as Dolt's do).
  - It survives renames and retypes, and a rebuild writes the commit's tags
    back (`bench/prolly_tags.sql`).
- **Layouts:** each table's layout lists the primary key and every column
  by tag.
- **Rows:** keyed by the primary key's values, with the other non-NULL
  columns as (tag, bytes) cells sorted by tag.  So a rename, a nullable
  `ADD COLUMN`, a retype that keeps the text, or dropping an all-NULL
  column leaves every row byte-identical.
- **Diff** (`bench/ws_diff.c`) reports:
  - objects added, dropped and changed;
  - columns added, dropped, renamed, retyped and with changed defaults, by
    tag;
  - rows inserted, deleted and updated.  A row counts as updated only if a
    value in a column both sides have changed; rows whose bytes changed
    only because of a schema change count as rewritten.
- **Merge** (`bench/ws_merge.c`):
  - **Objects** get a three-way merge by key.
  - **CREATE TABLE** merges item by item: columns by tag, constraints by
    name, the rest only if one side changed it.
  - **Layouts** merge column by column.
  - **Rows** merge by key; where both sides changed a row, they merge
    column by column.  A keyless table merges as a multiset, adding theirs'
    change in count to ours.
  - **One-sided rows** move into the merged schema: dropped columns'
    values are removed, and columns the other side added with a default get
    it.
  - **Sequences** keep the further position.
  - **Conflicts** are typed: object, table-ddl, column, column-name, key,
    table, row.
- **Constraint violations** surface when the merged commit is restored,
  which now collects problems instead of failing:
  - an object that never applies is reported as unapplied;
  - a row that breaks a CHECK or NOT NULL in the table's definition is
    reported, the constraint is set aside, and the other rows still load.

**DDL steps** (`bench/rebuild --ddl`, 5,000 keyed rows and 2,000 keyless).
Each step's diff must contain its expected lines, and each commit must
rebuild to itself:

| Change | Row data | Diff |
|---|---|---|
| rename a column | same tree | column renamed; 0 rows |
| ADD COLUMN, nullable, no default | same tree | column added |
| ADD COLUMN … DEFAULT 7 | changed | column added; 0 updated, 5,000 rewritten |
| retype int → bigint (same text) | same tree | column retyped |
| retype bigint → numeric(10,2) | changed | column retyped; 5,000 updated |
| drop an all-NULL column | same tree | column dropped |
| drop a column with values | changed | column dropped; 0 updated, 5,000 rewritten |
| CREATE INDEX | same tree | object added |
| change rows | changed | +1 −10 ~10 |
| drop a column and add it back | same tree | (same tag: same column) |
| keyless: add nullable, rename | same tree | column added / renamed |
| keyless: add with default | changed | 0 updated (rewritten) |
| keyless: change rows | changed | +2 −20 |

**Merge scenarios** (`bench/rebuild --merge`, 21 of them).  Each one:
1. builds a base commit;
2. makes theirs in a database rebuilt from the base, and ours in the
   original;
3. merges at the merge base;
4. restores the result.

Outcomes must be exactly those expected.  Each clean result must pass
amcheck, answer its checks, and capture back to the merged root value.

| Scenario | Outcome |
|---|---|
| different rows changed; theirs inserts | clean |
| same row, different columns | clean (merged by column) |
| same cell | conflict row acct 5 |
| delete vs update | conflict row item 7 |
| both add different columns (one with a default); ours inserts | clean; ours' new row gets theirs' default |
| rename vs update of that column | clean |
| both add the same column | clean (same tag) |
| same column name, different types | conflict column-name |
| both retype a column differently | conflict column |
| drop vs update of that column | clean (the drop wins) |
| both insert the same email | violation acct_email_key |
| parent deleted vs child inserted | violation item_acct_fkey |
| CHECK added vs violating update | violation score_small |
| UNIQUE index vs duplicating update | violation acct_name |
| same index name, different definitions | conflict object INDEX x |
| keyless: both insert the same row; theirs deletes | clean, counts added |
| both add an enum value | conflict object TYPE color |
| rename vs a new index on the old name | unapplied INDEX acct_name_idx |
| NOT NULL DEFAULT column vs ours' insert | clean; default filled |
| sequence advanced on both | clean; further position kept |
| both insert the next serial id | conflict row ticket 1 |

No clean merge needed normalizing.  Rebuilt and captured again, each gave
back exactly the merged root value, so the item-by-item CREATE TABLE merge
reproduces pg_dump's text.  The earlier checks still all pass: the rebuild
fixture, the streamed fixture (tags travel in the layout messages), and
every `--break` case.

**Findings:**

- **Keyless rows need an additive merge.**  Both sides inserting the same
  row makes the same new count on both, so a merge of values keeps one
  insert.  The counts must add instead.  Scenario 16 caught it.
- **Inline CHECKs make COPY all or nothing.**  One violating row emptied
  the table, which then broke a foreign key as well.  Setting the violated
  constraint aside and reloading reports only the real violation.  Scenario
  13 caught it.
- **Objects other than tables named columns** at first, so an index or
  view a branch created on a column the other branch renamed wouldn't
  apply.  They now reference tags (below).
- **Per-cell tags cost space.**  60-bit tags take 9 bytes per cell: the
  rebuild fixture's store grew from 55 to 75.5 MB (+37%).  The real format
  should be positional within the layout, mapping each tag to a slot, with
  trailing absent slots meaning NULL (as Dolt's tuples are), or use small
  per-table column numbers.
- **To decide:**
  - a drop wins over a concurrent update of the dropped column (scenario
    10), silently;
  - a column dropped and added again with the same name and type gets the
    same tag, so diffs treat it as the same column;
  - a default filled into rows the other side wrote is evaluated once, so
    a volatile default gives every such row one value;
  - a merge with violations leaves them in the working set to be
    resolved (as Dolt does); committing over unresolved ones is allowed
    here, and the violations go into the commit.

**Column references by tag in objects.**  Every object's DDL, and the key
of a column's comment, is stored with each column reference replaced by
`@{<tag>}`, and rendered with the commit's names on restore (`bench/ws_tags.c`).
A `~cols` entry maps each tag to its table and name, for every table
including partitioned ones; it merges by tag, so a rename on one side is
taken and two different renames conflict.
- **What counts as a reference:**
  - pg_dump writes relations schema-qualified, so an object's tables are
    known.
  - An identifier is a column reference when it names a column of such a
    table and isn't a function, type, qualifier, field of a composite
    value, string, comment or dollar-quoted body, or a name after a keyword
    like AS or INDEX.
  - `table.column` belongs to that table.  An unqualified name belongs to
    the nearest table named before it that has that column.
- **Why it's safe:** rendering a capture always gives the captured text
  back, so a reference read wrong costs only a rename's reach, never a
  rebuild.
- **Merged objects:** CREATE TABLE items are now keyed by the tag in the
  column line itself.  A rename changes only `~cols` and the layout; the
  table's object, and every index, view or constraint that uses the column,
  stays byte-identical.
- **The real implementation** would serialize the catalog's expression
  trees, whose column references are attribute numbers, mapped to tags,
  and needs no lexer.

New scenarios, all clean.  A branch adds each of these while the other
renames a column it uses; the result restores, passes amcheck, and its
definitions show the new names:
- an index on the renamed column;
- a view through a join and table aliases, and a materialized view;
- an expression index with a predicate;
- a foreign key to a renamed primary key, from a renamed column;
- a CHECK, and a trigger `UPDATE OF` the column.

Two branches renaming one column differently conflict.

**Constraint violations are stored** (`bench/ws_viol.c`), modelled on Dolt.
When a merged commit is restored, each violated constraint is:
- **deferred:** a foreign key or CHECK is added back NOT VALID, so rows
  written meanwhile are still checked; a unique constraint or index, an
  exclusion constraint or a NOT NULL waits;
- **found row by row:**
  - foreign key: child rows whose referenced row is missing;
  - CHECK: rows for which the expression is false;
  - unique (constraint or index, with its predicate): rows that share
    their key with another row;
  - NOT NULL: rows with the column NULL;
  - exclusion: recorded without rows;
- **recorded:** in `prolly.violations` in the working database (a row per
  violating row, keyed by its primary key values, and a row per constraint
  holding its object key and definition by tag), and so in the commit's
  `~viol` entry.  The diff reports `violation added|resolved`.

Resolving re-applies each deferred definition and clears what now holds.

| Scenario | Violations found | After a fix and resolving |
|---|---|---|
| both insert the same email | acct 5001, 5002 (acct_email_key) | the unique constraint is back |
| parent deleted vs child inserted | item 9001 (item_acct_fkey) | the foreign key validated |
| CHECK added vs violating update | acct 1 (score_small) | the CHECK validated |
| UNIQUE index vs duplicating update | acct 1, 2 (acct_name) | the unique index built |
| NOT NULL added vs NULLs written | acct 3, 4 (acct_name_not_null) | NOT NULL set again |
| unresolved: a CHECK and a unique constraint | item 10, 20 and acct 1, 5001 | (kept) |

In each:
- **Storing:** the working set with violations captures to a root value
  whose `~viol` holds them, and that root value rebuilds to itself.
- **Resolving:** the resolved working set has no violations, and its
  schema objects equal the merged commit's exactly.

Two new faults are caught: objects keeping column names (`--break
notags`, the rename scenarios fail), and violations not stored
(`--break noviol`).

### Moving the working set in place

Checkout, reset and applying a merge move a live database from one commit
to another.  Rebuilding into an empty database works but costs the whole
schema.  `bench/ws_move.c` moves it in place, in this order:
1. **Enum values** the target adds go in place, outside the transaction
   (Postgres can't use a new value in the transaction that adds it).
2. **Objects** the target lacks or changed are dropped.  Anything on a table
   the target drops goes with the table.
3. **Types, domains, functions and sequences** the target adds or changed
   are created.
4. **Tables:**
   - columns are renamed (through spare names, so swaps work);
   - each table the target drops is dropped and each it adds is created;
   - a table both have is altered in place where Postgres allows it:
     columns added at the end, dropped or retyped; defaults and NOT NULL
     changed; table constraints and storage parameters changed;
   - otherwise it is recreated: columns reordered, partitioning or other
     definition changes, or a type it uses dropped.
5. **Tags** are set as the target has them.
6. **Rows:**
   - a new or recreated table is loaded whole;
   - for the rest, only the row keys that differ are deleted and inserted;
   - a row whose bytes differ only because of the schema change is left
     alone: its shared columns agree, and each new column holds what the
     database filled it with.
7. **Statements that need the rows** run next: NOT NULL, constraints, then
   sequences and violations.
8. **Convergence:** until the database's objects equal the target's,
   whatever the target has and the database lacks is created, and anything
   extra dropped.
9. **Materialized views** are refreshed.

Writes run with `session_replication_role = replica`, so user triggers,
foreign keys and event triggers don't fire.

**The test** (`bench/rebuild --move`) moves one database through all 12
ordered pairs of the four fixture commits.  After each move it must capture
to exactly the target's root value, pass amcheck on every heap, B-tree and
GIN index, and answer the target's 90 checks (queries forced through each
named index, sequences, constraints).  `--break keepall`, which leaves
every changed row alone, is caught.

| Move | In place | Full rebuild | Changed keys | Rows deleted / inserted / kept | Where the time goes |
|---|---|---|---|---|---|
| load → rows changed | 1.2 s | 5.9 s | 12,230 | 9,515 / 7,118 / 0 | rows 0.45 s |
| rows → rows (03 ↔ 02, the DDL already behind) | 1.9 s | 5.7–6.0 s | 7,410 | 7,042–7,108 / 6,928–7,222 / 0–180 | rows 1.1 s |
| rows → DDL (01 → 02) | 5.6 s | 5.7 s | 20,119 | 0 / 138 / 19,981 | schema 3.6 s |
| DDL → rows (02 → 01, an enum value taken out) | 5.8 s | 5.9 s | 20,081 | 0 / 20,081 / 0 | objects 4.6 s |
| load → after DDL and rows (00 → 03) | 8.0 s | 6.0 s | 30,807 | 15,809 / 13,664 / 11,881 | schema 5.2 s |

- **Without DDL, a move costs the rows that changed:** 3–5× faster than a
  rebuild here, and the gap grows with table size.
- **Rows only rewritten by a schema change aren't written.**
  `ADD COLUMN … DEFAULT` makes 19,981 rows' bytes differ, but Postgres's
  fast default already gives them the value, so they are left alone.
- **DDL that Postgres implements as a rewrite costs O(table + its indexes)
  in place too:**
  - `ALTER COLUMN … TYPE` rewrites `kinds` and rebuilds its 40 indexes
    (3.6 s of the 5.6 s);
  - taking an enum value out isn't possible in Postgres, so the type is
    dropped and every table using it recreated (4.6 s, nearly all index
    builds).

  So checkout across DDL costs about what Postgres's own DDL costs.  The
  window's cheap checkout holds for row changes and for in-place DDL
  (renames, nullable or fast-default columns, constraints, indexes), not
  for table rewrites.  Avoiding that would take keyframes of the
  rewritten tables' pages, or copy-on-write working sets.
- **Merges apply in place:** all 14 clean merge scenarios are applied to
  ours' working set, and each captures to exactly the merged root value.
- **Bugs found:**
  - the object lists' keys aren't NUL-terminated, and a lookup treated
    them as strings;
  - indexes and constraints of a partition the target drops can't be
    dropped first;
  - a fillfactor change recreated the table where `ALTER TABLE SET` does.

  The move tests cover all three.
- **Not covered yet:**
  - moves under concurrent writers;
  - moves with violations pending;
  - the checks after a move run on a database with planner statistics from
    VACUUM ANALYZE (included in the times above, as it is for rebuilds).

## Crash safety and backups

**The design:** a chunk journal in the store, and the commit of record in
Postgres.  Dolt was the model:
- **From Dolt:** its chunk journal (length-prefixed, CRC32C records with
  root records among the chunks, one `fdatasync` per commit, a pre-zeroed
  tail, refusing to open on mid-log damage); its single root of record;
  and its incremental backups.
- **Where it differs:** Dolt owns all its state, so a commit is one root
  swap.  Here Postgres owns the working set, so the root of record lives in
  Postgres.  (Dolt's binlog is the cautionary case: written after the
  commit, not synced, not atomic with it.)

**The store** (`dirstore.c`):
- **Flushing:** a flush appends the new chunks and a root record (the root
  set and repo state) to `j-<id>.journal` and fdatasyncs it.  That's the
  store's commit point, with no new file per commit.
- **Records:** `[u32 length][kind][payload][crc32c]`.  The file is extended
  4 MB ahead with zeros, and a zero length ends the log.
- **Opening:** opening replays the journal after the manifest.
  - A torn tail is cut.
  - Damage followed by a valid root record isn't a torn tail, and the open
    refuses.
  - `dirstore_open_at` opens as of an earlier root record and cuts what
    follows.
- **Compaction:** compaction folds the journal into immutable chunk files
  and starts a new one.

**A Dolt commit, in order** (`bench/rebuild --durable`):
1. The barrier, and the stream applied up to it.
2. The commit's chunks and root record appended to the journal, then
   synced.
3. `INSERT INTO prolly.roots` (seq, state, root value, barrier LSN, journal
   end, the decoder's tables), in an ordinary transaction.  The commit
   exists from here.
4. Only then is the replication slot told the WAL up to the barrier may go.

Postgres never names a root whose chunks aren't durable, and the WAL needed
to redo a commit is kept until the commit is recorded.

**Recovery:**
1. Open the store as of the last row in `prolly.roots`, cutting any journal
   records past it.
2. Rebuild the decoder from that commit's root value and table map.
3. Replay from the slot, passing over transactions that commit at or
   before the recorded barrier.  Postgres's own crash recovery can move a
   slot's position back.
4. The recomputed commit is the same root value, by content addressing.

**Crash tests** (`make durability-check`, in a cluster of its own on port
5498).  Each case kills the committer, or Postgres itself, at a point in a
commit, sometimes followed by a power cut (the journal cut at a random
offset past the last recorded root).  It then resumes and finishes the
fixture.  The resumed run must record each step exactly once, as the
store's log holds it, with every stream root equal to a fresh capture's and
the store consistent.

| Case | Result |
|---|---|
| streamed, nothing durable (commit 1, and across the DDL commit 2) | recovered |
| journal synced, root not recorded (1, 2) | recovered |
| root recorded, slot not told (1, 2) | recovered |
| journal synced, then a power cut | recovered |
| root recorded, then a power cut | recovered |
| Postgres down hard mid-commit | recovered |
| Postgres down hard after the root was recorded | recovered |

The store's own tests cover 200 power-cut points, each cut clean and torn
mid-record, and all open as the last whole commit.  Damage in the middle of
the log is refused, and opening as of an older root is tested.

**Backups:**
- **Physical, with point-in-time recovery** (the backup suite):
  1. `pg_basebackup` after commit 1, with WAL archived, and the store copied
     then.
  2. Commits 2 and 3, and the store copied again with the journal last.
  3. Recovered to just after commit 2.

  Then:
  - the recovered database's last commit of record is 2;
  - the store copied with the base backup refuses to serve it;
  - the later copy opens as of commit 2, which rebuilds;
  - the recovered source's tables are commit 2's, apart from the unlogged
    table, which recovery empties.

  The rule: copy the store after the base backup, journal last.  The
  journal is append-only and content-addressed, so a later copy holds every
  earlier root's chunks, as long as compaction keeps what a retained backup
  needs.
- **Logical, incremental** (`--backup DIR`): after every commit the repo is
  pushed to a backup store, and only the chunks it lacks are sent.  Restored
  from the backup alone, the last commit rebuilds.

| Commit | Commit time | Journal sync | Root row | Source WAL | Journal added | Backup sent |
|---|---|---|---|---|---|---|
| load | 3.9 s | 0.32 s | 7 ms | 114 MB | 32.9 MB (0.29×) | 32.6 MB, 0.28 s |
| rows changed | 1.0 s | 13 ms | 6 ms | 18.5 MB | 4.5 MB (0.24×) | 4.4 MB, 0.04 s |
| DDL | 4.5 s | 76 ms | 6 ms | 133 MB | 28.3 MB (0.21×) | 28.1 MB, 0.26 s |
| rows changed | 1.8 s | 28 ms | 7 ms | 44 MB | 10.6 MB (0.24×) | 10.5 MB, 0.09 s |

**What this says about logging chunks through Postgres's WAL:**
- **The cost:** it would add the journal's bytes to the WAL, 21–29% more
  here.  This fixture's WAL is heavy (index builds, full-page images);
  workloads of small random updates write far less WAL per changed row, so
  the ratio would be higher (the disk benchmark wrote about 9 MB of chunks
  per 2,000 random updates).
- **What it would buy:** standbys and point-in-time recovery of the store
  with no extra steps.
- **What the journal plus root row costs:** one fdatasync and one small
  transaction per commit, and point-in-time recovery works as long as the
  store is copied after the base backup.

  The cheaper next step, if history must survive failover, is recomputing
  the store on a standby from the WAL, which already holds everything a
  commit is made from.
- **Not covered:**
  - compaction's retention rule for chunks that backups still need;
  - concurrent writers while crashing.

## Parents in a commit graph (severing to a real root)

**Decision (2026-10-29):** keep parents in the commits (`vc`), with the
saved-state change below.  The commit graph cost a little more space, and
lost `vc`'s refusal to re-fetch the seam commit, in exchange for a real
root at the cut.  The commit-graph engine (`vcg.c`, `cgraph.c`, its tests
and `bench/graphbench`) was then removed; it is in commit 8822996e6e1.

**The question:** when the window severs init <- a <- b <- c <- d <- e
down to c <- d <- e, can c become a real root without rewriting any
commit?  With parents inside the commit (`vc.c`), no: c's hash covers
"my parent is b", so c stays a commit whose parent is cut.  `vcg.c`
tries the other way.  It sits beside `vc.c`, so the two can be measured
against each other.

**The design** (`cgraph.h`, `vcg.h`):
- **Commits:** a commit's hash covers its root value, generation, a
  16-byte random nonce, time and message.  It does not cover its parents.
  The nonce keeps two commits with the same root and message distinct.
- **The commit graph:** parent edges live in a prolly map keyed by
  (generation, commit).  Its root hash covers every edge.  Keying by
  generation means new commits touch only the last leaves, and a sever
  touches only the first ones.
- **Sever:**
  - the cut commits' edges, plus the edges from kept commits into them,
    go into a segment graph, and its root hash H names the segment;
  - each kept commit loses its cut parents and records H;
  - c is then a root (no parents) whose history continues in H, and no
    commit hash changes.
- **Relink:** a segment comes back from any store only if it hashes to H
  and the commits it names hash to their names.  Relinking puts back the
  exact graph that was cut, down to the same root hash.  Segments go back
  newest first; a merge commit cut on both sides is refused until the
  later cut is back.
- **Sync:**
  - a ref is a head plus the sender's graph root, and edges are read
    through that root, so each chunk is checked against its hash;
  - an edge must agree with the generations at both ends (they are part
    of each commit's hash);
  - it must also agree with any edge the receiver already holds for that
    commit.  A root-with-seam on one side agrees with the full edge on the
    other.
  - Fetches stop at commits the receiver knows, and report detached or
    unrelated histories, as in `vc`.
- **What the repo saves:** branches, the graph root, the commits held, and
  only the segments it still holds or exported.  A discarded segment is
  found again by the hash its kept commit records (`vcg_segment_find`).
  So the saved state is the size of the window, however much has been
  cut.

**Tests** (`test/test_vcg.c`, 16 of them):
- nonces;
- the example above, where c becomes a root with the same hashes;
- relinking from the local store, from an archive, and from forged
  stores, each refused (another history's segment under H, another
  commit's bytes under a cut commit's name);
- newest-first ordering, and a merge commit cut twice;
- the merge base inside the window, and past the cut (`VCG_ERR_SEVERED`);
- merges;
- sync between repos cut at different depths;
- fast-forward-only push;
- forged edges on fetch: contradicting a known edge, a skipped generation,
  a head missing from the graph;
- detached and unrelated fetches;
- a clone carrying the cut and relinking it from the original's archive;
- save and reopen;
- the saved state not growing with history.

Each check was also disabled in turn to confirm a test fails.  The one
exception is hash verification when copying: the store re-hashes every
chunk it is given, so bytes under the wrong name can never land.

**Disk, against parents in the commits** (`bench/graphbench`).  It runs
the same seeded commits through both engines on the durable store; table
data comes out as identical chunks, so only history differs.

The first comparison was not like for like: `vc` saved a location for
every commit it ever had, plus every segment's commit list, and rewrote
all of it at every commit.  That came to 315 KB after 5000 commits and
808 MB written.  So `vc` now keeps the same state as `vcg`:
- a discarded segment's commits are forgotten, and only local and
  exported segments are saved;
- what it still knows of cut history is what its held commits name: their
  parents.  Those are rebuilt from the commits on open, so a transfer
  still stops at the seam commit and never re-sends it;
- deeper discarded commits become unknown, as in `vcg`.

With both on the same state:

| Workload | History, vc | History, vcg | Written, vc | Written, vcg |
|---|---:|---:|---:|---:|
| 200k rows, 200 commits x 2000 changes, window 20 | 1.7 KB | 2.5 KB | 820 MB | 821 MB |
| 20k rows, 5000 commits x 1 change, window 50 | 4.1 KB | 6.2 KB | 49 MB | 70 MB |
| same, never severed | 396 KB | 611 KB | 294 MB | 325 MB |
| 20k rows, 2000 commits x 10 changes, 32 branches, window 10 | 27 KB | 39 KB | 143 MB | 159 MB |

"History" is the live commits, graph and saved state at the end.
"Written" is every byte put over the run.

- **`vc` is a little smaller everywhere.**  Its commits carry about 13
  bytes more each (parents in place of the nonce).  The graph costs more
  than that: about 43 bytes per commit live, and each commit rewrites the
  graph's last leaf and its path, about 4 KB written per commit with 4 KB
  chunks.  A smaller chunk target for the graph would cut the writes.
- **Disk:** table data dominates.  On disk the two differ by under
  0.3 MB in every run.
- **So the decoupling buys semantics, not space:** a real root at the cut
  with no hash changed, and relinking checked against a recorded hash.
  The saving in the first comparison was the state diet, which now
  applies to both.

**Left open:**
- A receiver accepts a new commit's edges on the sender's graph root,
  just as it accepts the sender's head.  A sender lying about history the
  receiver has never seen is not detectable.
- The window is recomputed on every sever, not tracked incrementally.
- `vcg` has no commit images, pins, `VC_WINDOW_ONLY`, deepen or pull yet.
- Fetch can bring back history this repo discarded, if a sender still
  has it.  `vcg` doesn't even stop at the seam commit: `vc` reads cut
  parents from its kept commits, but `vcg` has only the segment hash.

## Millions of commits in the window

**The question:** do version-control operations stay cheap when the
window holds one deep history of millions of commits?  That is what a
high-rate `dolt tx commit` mode produces.  The earlier scale tests were
wide (1M branches of one to three commits each) or shallow (400 commits).

**`bench/deepbench`** runs on the durable store:
- a 100k-row table, 1 KB chunks (the table's own writes per commit do not
  depend on the window), and 4 row changes per commit;
- 1M commits on main, all kept;
- a full sever pass every 1000 commits;
- two side branches for merge bases: one off the first commit, one off ten
  commits back.

**What was O(history) per commit, and the fix.**  The saved repo state held
a location for every known commit, rewritten whole at each save.  Now:
- locations are a prolly map in the store, keyed by commit;
- changes wait in memory and are written at save, so a save costs
  O(changes x log W);
- segment ids are stable, so locations never need renumbering;
- a sever that walks the window records which cut commits the kept ones
  name, so discarding no longer needs an O(W) walk to rebuild them.  After
  an incremental sever, it falls back to rebuilding them lazily.

**At 1M commits**, before and after:

| Measure | Before | After |
|---|---:|---:|
| Commit (edit + commit) | 90 us | 85 us |
| Save repo state | 35 ms, 11.3 MB written | 37 us, 5.7 KB written |
| Saved state chunk | 20.5 MB | 0.3 KB |
| Reopen from saved state | 42 ms | < 0.1 ms |
| Full sever pass | 227 ms | 238 ms |
| Log of 100 | 123 us | 103 us |
| Merge base, ten commits back | 2 us | 1 us |
| Merge base, the whole history back | 538 ms | 491 ms |
| Row diff, 1 commit back (4 rows) | < 0.1 ms | < 0.1 ms |
| Row diff, 1000 back (3,922 rows) | 3.6 ms | 3.4 ms |
| Row diff, 500k back (all 100k rows) | 5.3 ms | 4.3 ms |
| Disk | 39 GB | 31 GB |

- **The fix makes saving independent of history.**  Before, a save every
  commit would have written about 11 TB over the run, so the baseline
  saved every 1000 commits; the after run saved every commit.  The 8 GB of
  disk saved is the old state chunks.
- **Flat with history:** commit, save, reopen, short logs and merge bases,
  and diffs.  A diff costs the rows that differ, never the commits between:
  500k commits back costs what the whole table differing costs.
- **Still linear in W:**
  - The full sever pass walks the window (about 0.24 us per commit held).
    Run it every W/k commits rather than every 1000 and it costs O(k) per
    commit, however deep the window is.
  - A merge base far back walks every commit between, as in git and Dolt:
    0.5 s across 1M commits.  Ancestor skip pointers (a commit naming its
    ancestor at generation g - 2^k) would make it logarithmic, if
    long-lived branches merging across millions of commits matter.
- **Memory (about 19 GB in both runs) is the store, not the repo.**  The
  journal is memory-mapped, and the store keeps an index entry per chunk
  (about 20 chunks per commit here, since every version of the table is
  kept).  That cost is per chunk retained, whatever the window.

**Keyframes don't change with commit count.**  Checkout cost follows the
rows that differ: the prolly diff found 100k differing rows in 4 ms.  The
Postgres side is what costs: replaying 100k rows at about 8 us each is
about 0.8 s.  That is where a keyframe could pay.  Its break-even point R
(rows changed since the last keyframe) still needs measuring against the
real page-restore path, not the libpq harness's 150 us per page.  Nothing
in the window's depth changes R.

## The window as floors and ceilings

**What it is.**  The window is set by commits, change size or time, alone
or combined, as floors (keep at least) and ceilings (keep at most):
- **floors:** `depth` (generations from a head), `keep_bytes`,
  `keep_seconds`;
- **ceilings:** `max_commits`, `max_bytes`, `max_seconds`;
- **the rule:** a commit is kept when any floor covers it and no ceiling
  cuts it, so a ceiling wins over a floor.  With no floor set, the
  ceilings alone decide.  Branch heads are always kept;
- **change size** is new-chunk bytes: the chunks a commit's root has that
  its first parent's lacks.  Content alone decides it, and it costs what
  changed to compute at commit time.

**How it is cheap.**
- **What a commit carries:** its generation, cumulative change bytes and
  time (wall-clock microseconds, raised past every parent's), and the same
  three for each parent.  All three only fall along history.  So a head's
  window is one walk that stops at the first commit out, and a parent no
  floor keeps is never read.
- **When to sever:** `vc_sever_due` says a sever is due once the commits
  since the last one reach a tenth of those held, or their change bytes a
  tenth of the smallest byte limit, or the time a tenth of the shortest
  time limit.  Severing when due keeps the window within W/9 over a
  ceiling, at a cost per commit that does not grow with W.
- **The tests** (`test/test_window.c`) check:
  - every limit as a floor and as a ceiling, and exact byte boundaries;
  - change bytes against a direct closure diff, and times always
    increasing;
  - the overshoot bound, and the counters across a reopen;
  - a randomized run of branches, merges, clock jumps and policies,
    against the window computed straight from the definition.

**At 1M commits** (`bench/deepbench --due`, saving every commit; a 100k-row
table, 4 changes a commit, about 15 KB of change bytes each):

| Window | Held at 1M | Commit + sever | Save | Severs, 100k to 1M |
|---|---:|---:|---:|---:|
| everything | 1,000,004 | 109 us | 35 us | 22, 169 ms each |
| at most 100k commits | 110,133 | 112 us | 28 us | 81, 196 ms each |
| at least 0.5 GB, at most 1.5 GB | 33,063 | 109 us | 25 us | 288, 44 ms each |
| at least 10 s, at most 30 s | 54,835 | 115 us | 26 us | 175, 85 ms each |
| at least 5 s, at most 200k commits and 1 GB | 28,353 | 108 us | 24 us | 308, 41 ms each |

- **Flat.**  Commit plus amortized severing is 98-115 us from 1k to 1M
  commits in every policy.  Logs, short merge bases, diffs and reopening
  do not move.
- **What a ceiling does.**  It only bounds what the floors keep.  With a
  10-second floor under a 30-second ceiling, the window is the last 10
  seconds (about 55k commits here); a ceiling over a smaller floor never
  binds.
- **The cost of severing** includes discarding, which frees the cut
  commits' chunks: about 2 us a commit held.

## Low and high marks: sliding, tumbling, in between

**What it is.**  The floors and ceilings above are the window's high mark.
Low marks (`low_commits`, `low_bytes`, `low_seconds`) say what a cut
leaves:
- **no low mark:** the window slides, as before: every sever cuts each
  head's history back to exactly its window;
- **with low marks:** a head's window grows until it reaches its high
  mark, meaning it would let go of a commit still held.  The sever then cuts
  that head's history back to its low mark (the window with the low marks
  as further ceilings), and it grows again;
- **`low_commits = 1`** keeps only the head: a tumbling window, history in
  batches.  A low mark between gives hysteresis ("grow to 1M commits, cut
  back to the last 100k"); one at or past the high mark slides;
- **per branch:** a head short of its high mark keeps its whole window, so
  each branch fills and empties on its own.  A low mark wins over a floor,
  as a ceiling does; low marks with no high mark are refused.

No state is added: what is held says where each head is in its fill, and
`vc_sever_due` is unchanged (a sever is about a tenth of what is held, so
after a cut to one commit severs are frequent but tiny).

**What it buys.**
- **REWRITE mode:** a cut writes again only what the low mark keeps.  A
  tumbling cut writes one commit, the head, which becomes the root; a
  sliding sever writes the whole window every time.
- **Batches:** each cut is one segment holding exactly one batch, a clean
  unit to export, archive or back up; each batch's root restores it.
- **The cost:** right after a cut, history is only the low mark: no diff or
  revert past it.  What is guaranteed is the low mark, not the high one.

**Tests** (`test/test_window.c`, `test/test_rewrite.c`):
- a line of commits severed after each: the window holds h, then h + 1, or
  past N the low mark, for tumbling, hysteresis, a high mark of one, a low
  mark at and past the high mark (which slides);
- a depth floor as the high mark, cut back to a low mark under it;
- branches filling on their own (a busy branch cut, a quiet one keeping
  its whole window and the shared history), a pinned commit through a cut;
- randomized branches, merges, clocks and marked windows of every limit,
  against the window computed from the definition;
- severing when due with a tumbling window: within a tenth over the high
  mark, severs bounded;
- REWRITE: each cut writes exactly the low mark's commits again, the
  oldest a root at generation 1, and nothing between cuts;
- low marks alone refused.
- **Each part was broken in turn, and each break was caught:** never
  reaching the high mark, always reaching it, byte and time low marks
  ignored, one head's cut applied to every head.

**At 1M commits** (`bench/deepbench --due --max-commits 100000`, with and
without `--low-commits 1`, in both modes; the four ran side by side, so
absolute times are about 1.7x a lone run's, and only the comparison
counts):

| | Commit + sever | Severs, 100k to 1M | Each | Held at 1M |
|---|---:|---:|---:|---:|
| KEEP, sliding | 214 us | 81 | 336 ms | 110,133 |
| KEEP, tumbling | 225 us | 810 | 47 ms | 91,494 |
| REWRITE, sliding | 236 us | 81 | 595 ms | 110,133 |
| REWRITE, tumbling | 226 us | 810 | 47 ms | 91,494 |

- **REWRITE:** tumbling takes the rewrite out of severing (a cut writes one
  commit), so its severs cost what KEEP's do, and commits are about 4%
  cheaper than sliding.  The long pause is gone: no sever over 50 ms on
  average, against 595 ms.
- **KEEP:** tumbling is about 5% dearer.  `vc_sever_due` fires at a tenth
  of what is held, so after a cut it fires often, and each of those severs
  walks the window twice (once to see whether the high mark is reached)
  and mostly cuts nothing.  The total walking matches sliding's; the second
  walk is the extra.
- **To improve:** with low marks the due rule could wait for the high mark
  itself (a head reaches max_commits only once the commits held do), so
  severs would come once per batch.  Not done yet.

## Keyframes: replay or restore

**The measurement** (`bench/kfbench`, 1M rows with two indexes, every move
verified by row checksum and amcheck).  Each change shape moves back to
the base state by replay (one upsert of the rows) or by restore (the pages
that differ, read by the server from a file and logged 32 to a WAL record
by `prolly_put_pages_file`).  Timed after a checkpoint (cold) and without
one (warm), under each wal_compression.

| Change | off | pglz | lz4 | zstd |
|---|---:|---:|---:|---:|
| 10k sparse rows, cold: replay / restore ms | 255 / 192 | 1,045 / 949 | 192 / 146 | 309 / 252 |
| 10k sparse rows, warm | 97 / 182 | 94 / 919 | 98 / 144 | 93 / 254 |
| 100k clustered rows, cold | 970 / 78 | 1,398 / 468 | 964 / 73 | 986 / 122 |
| all 1M rows, cold | 8,535 / 385 | 10,535 / 1,414 | 8,057 / 254 | 8,036 / 404 |
| WAL of that restore | 181 MB | 43 MB | 47 MB | 26 MB |

**What decides it.**  Both paths write the pages that differ, so the
model is:
- `replay = F_r + a·rows + b_cold·cold_pages + b_warm·warm_pages`;
- `restore = F_s + c·pages`.

Measured with lz4: a ≈ 7.6 us/row; b_cold ≈ 19 us and b_warm ≈ 3.7 us a
page; c ≈ 10 us a page; F_s ≈ 8-17 ms.
- **Cold:** replay pays more per page than restore (a full-page image on
  first touch plus heap and index work), so restore wins past a few
  hundred to a few thousand rows.
- **Warm:** the pages already have their image since the checkpoint, and
  replay wins sparse changes up to about 10k rows.  Dense or large changes
  restore regardless: 100k clustered rows 73 ms against 964, the whole
  table 254 ms against 8 s.
- **How the change is spread** matters less than expected.  Size and
  checkpoint recency decide most of it.

**WAL compression is part of the design.**
- **lz4:** the fastest restore and about 4x less WAL than off, with replay
  unchanged.  Recommend it.
- **zstd:** the least WAL, but slower.
- **pglz:** 3-5x slower on restore.
- Costs are kept per setting, matched to the server's.

**The planner** (`kfplan.c`, behind an interface):
- **Inputs:** rows (prolly diff count), pages (image diff and the WAL),
  and warm pages (written since the last checkpoint).
- **Self-calibration.**  `kf_calibrating_planner` refits both models from
  every move it observes:
  - decaying least squares weighted by 1/time;
  - costs kept non-negative;
  - the measured costs act as a prior worth one observation.
- **Exploration.**  It learns only from the path it takes, so a path not
  taken for 50 decisions is tried on the next small move (under a quarter
  of the average).  That is cheap, and it is what pins fixed costs.
  Near-tie exploration was tried and removed: nothing needed it.

**Accuracy** (`test/test_kfplan.c`):
- **The measured moves, each predicted from the others:**
  - off: 14/16 right, 0.7% of the best time lost;
  - pglz: 16/16, 0%;
  - lz4: 14/16, 1.2%.
  - The misses are the 1,000-row cold moves at the break-even, 6-10 ms
    each.
- **A simulated server, starting from another server's costs:**
  - 0.7% lost, against 9.7% for a planner that keeps those costs;
  - late decisions 456/458 right (those where one path is 20% and 1 ms
    cheaper).
  - After a switch to pglz-like costs: 370/370, 0.06% lost.
  - Without exploration, from a prior that hides replay: 311/455 right,
    against 452/455 with it.

**End to end** (`bench/winbench --planned`, 1M rows, a keyframe at each
commit by the capture rule, 20 checkouts, each verified row by row against
the prolly tree):
- **Calibration worked:** the first checkout restored, estimated at 208 ms
  and taking 1,112 ms.  winbench's restore path sends pages as bytea, one
  record each.  The planner then replayed every later checkout, its
  estimates within about 20% of the time taken.
- **Planning cost too much:** the whole checkout grew from 0.3 to 2.3 s
  while the moves took 70-320 ms.  Counting candidate pages scans the WAL
  since the working set's base, a range that only grows.  The engine
  needs the dirty-page set maintained as the WAL is decoded, so a plan
  costs O(1).
- **Keyframes cost storage:** about 40 MB of new image chunks a commit
  here, 1 GB live across a 20-commit window.

## Real keyframes against replay, at a distance

**The question.**  The measurements above restored pages from saved copies
of the table's files, not from keyframes, and the one end-to-end run with
real keyframes restored through libpq at about 150 µs a page, so no
keyframe ever paid for itself.  Do real keyframes beat replaying rows once
the target is far enough back?

**What changed in the harness** (`bench/winbench`):
- `--restore-dir DIR`: a restore reads the target keyframe's pages from its
  chunks, writes them to a file there, and the server reads them from it,
  `XLR_MAX_BLOCK_ID` pages a WAL record (`prolly_put_pages_file`).  Writing
  the file is part of the restore's time.
- The bench server used `wal_compression = pglz`, which made the server's
  page writes about 62 µs a page.  With lz4 they are about 9 µs, the
  mechanism's speed.

**The run** (2026-10-30): 1M rows (23,674 pages), 40 commits of 20,000 random
row changes each, a keyframe at every commit, lz4, 30 checkouts to random
commits by restoring the target's keyframe and 30 by replaying rows, every
one verified against the prolly tree.  The keyframes are the prototype's
form: page chunks with page and relation maps, taken incrementally on
the commit's path, and **not frozen** (raw pages, no snapshot).

| Commits back | Replay | Restore the target's keyframe |
|---|---:|---:|
| 1 | 324 ms (19k rows) | 1,205–1,247 ms |
| 4 | 722–989 ms (74k rows) | 1,210–1,466 ms |
| 10 | 1,188 ms (180k rows) | 1,086–1,277 ms |
| 19 | 1,744–2,216 ms (313k rows) | 1,217 ms |
| 26 | 2,496 ms (418k rows) | 1,145 ms |
| 38 | 3,307 ms (574k rows) | 1,423 ms (37 back) |

- Replay grows with the rows that differ, about 6 µs a row.
- Restore is flat: 20,000 random changes already touch about 19,000 of the
  table's pages, so nearly every page differs one commit back, and the pages
  that differ can't exceed the table's.  This spread is the worst case for
  keyframes.
- The crossover is about 10 commits (180k rows); at 38 back the keyframe is
  2.3× faster, and the gap widens with distance.
- Of a restore's ~1.2 s, about 550 ms is the restore itself (reading pages
  from the keyframe, about 200–250 ms; the server writing them, about
  200–260 ms).  The rest is moving the working set's base point after it,
  a checkpoint in this prototype.
- Storage: about 200 MB of new keyframe chunks a commit, 8.4 GB live after
  40 commits.  A keyframe at every commit can't scale; spacing must bound it.

**The planner choosing** (same history, `--planned`, calibrating from
lz4-shaped priors): it replayed the near targets (1–3 commits back, 340–625
ms) and restored the far ones (from about 9 back, 1.1–1.9 s, where replay
would take 2–3.3 s), its crossover learned at about 6–9 commits.  A few
choices were its exploration: two restores at 4 back, one replay at 19,
one slow restore of 4.1 s.

**Under each wal_compression** (the same run repeated, 2026-10-30; medians
in ms of the checkouts in each band of distance):

| Commits back | off: replay / restore | pglz | lz4 | zstd |
|---|---|---|---|---|
| 1–3 | 694 / 1,403 | 727 / 2,313 | 634 / 1,248 | 637 / 1,367 |
| 4–9 | 934 / 1,589 | 949 / 2,840 | 789 / 1,354 | 808 / 1,506 |
| 10–19 | 2,130 / 1,495 | 1,701 / 2,662 | 1,787 / 1,311 | 2,008 / 1,375 |
| 20–29 | 2,446 / 1,553 | 2,214 / 2,938 | 2,417 / 1,447 | 2,293 / 1,521 |
| 30+ | 3,569 / 1,706 | 3,035 / 3,029 | 2,985 / 1,485 | 3,240 / 1,655 |
| Server writing the pages, mean | 372 | 1,668 | 239 | 457 |

- Replay barely depends on the setting; restore does, because it logs
  every page whole.
- Under lz4, off and zstd, restore wins from about 10 commits back, by
  about 2× at 30 and more.
- Under pglz, restore is about 7× slower to write and never wins within
  38 commits: keyframes need lz4 (or zstd), and the planner's costs must
  be kept per setting.

**What this shows and what it doesn't.**  The prototype's keyframes, without
freezing, beat replay past a crossover, and the planner finds it.  It does
not measure frozen keyframes, keyframes taken in the background, or the
production restore by sharing block maps, which should be flat at
milliseconds rather than at the cost of the pages that differ.

## Keyframes across the freeze horizon

**The hazard.**  A keyframe is pages, and a heap page names transactions:
each tuple's xmin and xmax, or a multixact.  The server can resolve those
only within its horizon:
- per relation, `relfrozenxid` and `relminmxid`;
- the commit log (`pg_xact`) and `pg_multixact`, truncated behind
  `datfrozenxid`;
- and xids compare modulo 2^32.

Put back a raw page from behind the horizon, and every read of it fails.
A second hazard: a page captured while another transaction is in flight
names that transaction, and once it commits, a restore shows its rows,
though they are not part of the commit.

**The fix: frozen keyframes.**  `prolly_get_pages_frozen` (in
`contrib/prolly_pages`) reads pages with every heap tuple resolved against
the caller's snapshot, which is the commit's exported snapshot, imported.
- **Visible tuples:** frozen, xmax cleared, ctid pointing at themselves.
- **Each HOT chain** keeps at most its one visible member.  A root that is
  not visible becomes a redirect to it, so index entries, which point at
  roots, still find it.
- **The rest:** a root with nothing visible becomes dead, where indexes
  may still point; dropped heap-only tuples become unused.
- **Index and visibility-map pages** are copied as they are.  B-tree pages
  name no 32-bit xids, and the visibility map's bits stay true, since a
  page all-visible before is all-frozen after.
- **So a frozen image names no transaction.**  It is valid however far the
  horizon moves, and it holds exactly the commit's rows, whatever was in
  flight.

**Tests** (`bench/xidbench`, a throwaway cluster of its own, about 5 s).
Each case captures raw and frozen images of a table with:
- several inserting transactions;
- HOT and non-HOT updates;
- deletes and TOAST.

It then restores each and checks:
- rows by checksum against the capture's snapshot;
- `verify_heapam` with TOAST;
- `bt_index_check` with heapallindexed;
- counts through each index;
- that a tuple without an updater points at itself;
- VACUUM.

| Case | Raw image | Frozen image |
|---|---|---|
| 3M xids later, every database frozen (template0 too), commit log cut | fails 5 of 6 checks: "could not access commit status of transaction", "xmin precedes oldest valid transaction ID" | passes all |
| A transaction in flight at capture commits afterwards | shows its rows | exactly the commit's rows |
| Two sessions hold share locks (1,847 multixacts on the pages) | n/a | no multixact or xmax left; passes all |
| Inserts, updates, deletes and VACUUM after a frozen restore | n/a | passes all |

Each part of the transform was broken in turn, and each break was caught:
- xmin not frozen;
- xmax kept;
- the root not redirected;
- invisible tuples kept;
- dead roots kept;
- ctid not reset;
- no transform at all.

**Cost:** freezing in the capture is cheaper than reading pages one SQL
call at a time.  16k heap pages took 34 ms frozen in one call (about 2 us
a page), against 83 ms through `get_raw_page`.

**What follows for the engine:**
- every keyframe is captured frozen, as of the commit's snapshot, and
  never as raw pages;
- the window can keep keyframes as long as it keeps commits;
- like TRUNCATE, a restore is not MVCC-safe for snapshots older than it;
  checkout holds an exclusive lock, and the spec says so.

## Version control runs no user code

**The rule.**  A checkout, merge, move or rebuild reproduces rows and
objects that commits already hold, and those include whatever triggers and
rules did when the rows were first written: audit rows, counters,
rewritten values.  Running them again would apply them twice, and a
trigger that raises would block the operation.  So version control runs
none of the user's triggers, rules or event triggers.  (Dolt's merge and
checkout run none either.)  Triggers are versioned like any other object,
enable states included: pg_dump's entry for a trigger, rule or event
trigger carries its ENABLE ALWAYS, REPLICA or DISABLE.

**What replica mode left on.**  `session_replication_role = replica`
silences ordinary triggers and rules, but not:
- triggers and rules enabled ALWAYS;
- those enabled REPLICA, which replica mode switches on;
- event triggers enabled ALWAYS or REPLICA.

**How** (both are in the prototype):
- **Rows, below SQL (C).**  `prolly_apply_insert` and `prolly_apply_delete`
  (in `contrib/prolly_pages`) take COPY text and write through the
  executor's simple apply paths, as logical replication does.
  - Nothing user-defined runs: there is no trigger descriptor, and the
    writes go below the rule rewriter.
  - Indexes and constraints are kept.
  - Identity columns and stored generated columns take the committed
    values (see the next section).
  - A delete finds its row through the replica identity index, or for a
    keyless table by every column, one copy a line.
  - In-place moves now write every insert and keyed delete this way.
- **Everything else, by transaction (B).**  `prolly.quiet()` switches off
  every ALWAYS and REPLICA trigger, rule and event trigger, and sets
  replica mode, inside the caller's transaction; `prolly.unquiet()`
  switches them back on before it commits.
  - No other session ever sees anything off, and the catalog ends as it
    began.
  - Event triggers go off first and come back last, since switching the
    others is DDL.
  - Re-enabling goes by object id and only for what is still off: an
    object the operation dropped stays dropped, and one it made again or
    set itself keeps that state.
  - Every move transaction, the restore and the harness's own install run
    this way.

**Tests:**
- **`bench/trigger_fixture`** (`make trigger-check`) passes in every mode:
  rebuild, stream, all 12 moves, and 2 merges applied in place.  It has:
  - BEFORE and AFTER row triggers, statement, TRUNCATE and deferred
    constraint triggers, each ALWAYS, REPLICA or default;
  - a BEFORE trigger that rewrites rows, and one that refuses to delete
    locked rows;
  - DO ALSO rules and a conditional DO INSTEAD rule;
  - ALWAYS and REPLICA event triggers logging into an unversioned table;
  - identity and generated columns, and a keyless table;
  - commits that change enable states, drop a trigger and add one.
- **What the checks show:**
  - every move captures to exactly its target's root, so no extra audit
    row, rewritten value, counter step or diverted row;
  - the enable states match, and the guard trigger still refuses deletes;
  - no user event trigger logged anything;
  - in the merges, rows from the other branch carry exactly one run of the
    row rewriter.
- **`bench/quiet_test.sql`** checks the functions directly: what goes off,
  what comes back, and objects remade, set or dropped in between.
- **Each of these was broken in turn, and each break was caught:**
  - the apply functions firing triggers;
  - quiet doing nothing, or missing REPLICA triggers, event triggers or
    replica mode;
  - unquiet doing nothing, missing rules, or overriding the operation's
    own states;
  - the install not quiet.

**Cost.**  The C apply runs at about SQL replay's speed, -10% to +14%
across shapes, with the same WAL.  Heap, index and WAL writes dominate,
not parsing and planning, so its value is that no user code can run.

**A Postgres bug it exposed.**  Inserting a row at a time lays rows out
differently from COPY, and in one order the main fixture's SP-GiST `inet`
index loses a row.  The move test catches it: an `ip <<= ...` query
answers one row short through `kinds_ip_spgist`.
- `bench/spgist_inet_repro.sql` reproduces it with 270 inet values (IPv4
  and IPv6 mixed), plain COPY and CREATE INDEX, and nothing of ours.
- GiST and B-tree indexes on the same rows are right.
- `make rebuild-check` fails on those three moves until it is fixed
  upstream.

## Identity and generated columns

**Decision (Dolt's).**  A stored generated column is data: its values are
in the commit like any other column's, as computed when the rows were
written.  Wherever version control writes rows back (rebuild, stream
capture, checkout, every move), they are copied, never computed again.
Only a row a merge makes is computed:
- a row both sides changed and the merge combined by column;
- a row one side wrote under another expression than the merged schema's,
  or without the column.
The generated column itself never conflicts.  If its expression fails on
the merged values (a division by zero neither side wrote), the row is a
`generated <table> <key>` conflict.  Dolt does the same: stored generated
columns live in the row, and `withGeneratedColumns` recomputes them only
for rows the merge resolved.

**Why copy.**
- Moves are exact.  `CREATE OR REPLACE FUNCTION` on a function a
  generation expression calls does not rewrite the table: older rows keep
  the old body's values.  Computing them again at checkout would give a
  working set that differs from the commit.
- No user code: a generation expression may call any user function marked
  IMMUTABLE.  Merge is the one exception, and only for the rows it makes:
  the merge is writing new rows, as the user would.  (In MySQL, and so in
  Dolt, a generated column cannot call a stored function, so its merge
  runs no user code at all.)

**What changed.**
- **Layout:** a column is `s` stored, `g` stored generated or `v` virtual.
  `g` columns are in the row's values, its COPY text and its diffs; `v`
  columns hold nothing.
- **Row apply** (`prolly_apply_insert`) parses fields itself, since COPY
  refuses generated columns.  It builds Postgres's generation plan and
  takes out each column it was given, so those are copied and a column not
  named is still computed.  Every ordinary column must be named; a virtual
  one cannot be.
- **Restore** loads a table with `g` columns through row apply (plain COPY
  otherwise, which is faster).
- **Stream capture** publishes stored generated columns
  (`publish_generated_columns = stored`).
- **Moves** replace a changed function in place (`CREATE OR REPLACE`): a
  drop would take every generated column that calls it.  If it cannot be
  replaced (its result type changed), it is dropped and the tables that
  call it are recreated.
- **Identity columns in moves:** a changed identity is dropped with
  `DROP IDENTITY`, not a sequence drop, which Postgres refuses.  A
  recreated table gets its identity back before the sequences are set.
- **Merge:** a `WsEval` gives the merge its evaluators: defaults, as
  before, and now generated columns.

**Tests.**
- **`bench/gen_fixture`** (`make gen-check`) passes in every mode:
  rebuild, stream, all 20 moves, and 5 merges applied in place.  It has:
  - identity columns ALWAYS and BY DEFAULT, with their own start and
    increment, altered and restarted;
  - stored generated columns that call a user function (which counts its
    calls in an unversioned sequence), in an index, under a CHECK, NULL;
  - a virtual column, and a keyless table with a generated column;
  - a commit that replaces the function, so older and newer rows hold
    different bodies' values;
  - expressions replaced, a generated column added, one made plain.
- **What the checks show:**
  - every rebuild and move captures to its commit's root, and the call
    count where version control wrote stays at zero;
  - the identities' sequences are where the source left them;
  - merges: sources changed on both sides, an expression replaced against
    new rows, a column added against new rows, the function replaced
    against a changed row (kept as written), and a failing expression (a
    conflict).
- **Each was broken in turn, and each break was caught:**
  - row apply computing the values: the call count, and after the
    function is replaced, a different root;
  - the merge copying from a side instead of computing (`noregen`);
  - the stream not sent generated columns (`nogenpub`).
- **Bugs it found in moves:** the function replace, and identity drops and
  re-adds, above.  Without them the fixture's moves fail.

**Still user code in row replay** (not changed here): CHECK constraints
and domain checks run on every row and can call user functions;
expression and partial indexes must call theirs to build entries; user
types' input functions run.  The rows come from a valid commit, so the
checks could be skipped; the indexes cannot be, except by page restore.

## Unlogged tables, large objects, replication, history security, statistics

The remaining Postgres features against version control, decided
(2026-10-29) and tested.

**Unlogged and temporary tables, large objects.**
- An unlogged table in a versioned schema is versioned by definition, never
  by contents: Postgres empties it after a crash, and history must not hold
  what a crash erases.  Capture (logical and stream) commits its definition
  with no rows; a move empties it, as a crash would; a new branch, made by
  `pg_dump --no-unlogged-table-data`, gets it empty.
- Temporary tables are a session's and never versioned; they work on a
  branch, even under a versioned table's name.
- Large objects stay shared: `pg_largeobject` is addressed by object id,
  not by schema.  A branch reads another's.  Versioned binary data belongs
  in `bytea` columns.
- Tests: `check_unlogged` on every commit (definition present, no rows) and
  every rebuilt or moved database (empty); the `unlogged` break (rows
  committed) is caught; experiment C's branch and large object checks.

**Logical replication downstream.**
- Anything that changes a branch's contents reaches the replication stream
  as row changes: every move of every fixture is decoded, and the stream
  carries exactly the rows the move wrote (200 for 200, 266 for 266).
  Moves never swap files, so a subscriber follows a branch.
- A publication belongs to the branch it was made on (its tables are that
  branch's relations).  A subscription reads the publication's tables from
  the publisher, so it must connect on that branch (`options=-c
  catalog_branch=N`); connected on no branch, it covers no tables.
  Tested end to end: a subscriber database receives branch 2's row and not
  branch 1's.

**Row security through history.**
- History is read under today's security: the reading branch's current
  row security policies and current SELECT grants (table and column), not
  those the commit was made under, which may have been looser.
- `ws_history_table` reads a table as of a commit into a scratch table with
  the commit's columns and rows and the current table's policies and
  grants; queries run as the reader.  If a current policy cannot apply to
  the commit's columns, the read is refused and nothing is left behind.
- `rebuild --history-rls`: a commit made under a policy that showed every
  row, read as alice, shows only her rows; the secret column is denied by
  today's column grants; a diff shows alice her own change and bob his;
  a commit before the column today's policy reads is refused.  The
  `oldpolicies` break fails every one of these.

**Statistics after moves.**
- Statistics are a branch's own and never committed.  A new branch copies
  its parent's (`pg_dump --statistics`): column statistics and row
  estimates match without an ANALYZE.
- A move made of row changes raises the tables' change counters, so
  autoanalyze takes them up: checked after every move.
- A page restore writes rows without row changes, so it counts the live
  tuples it wrote as inserted (`prolly_put_pages`): restoring 5,000 tuples
  counts 5,000 changes (`bench/stats_test.sh`; without the counting, 0).

## History mode REWRITE: a window and nothing else

**What it is for.**  Some users want history only for a window: no
remotes, earlier windows restored from backups of earlier roots.  For
them, a sever squashes.
- **What goes:** the cut history goes at once.  No segments, boundaries or
  record of the cut commits remain, and their chunks are freed.
- **What is rewritten:** every kept commit with cut history beneath it is
  written again without it.  The oldest kept commits become true roots,
  and their descendants get new hashes.
- **The cost:** the user takes that rewrite as the price of the mode.

**How** (`vc_set_history_mode(r, VC_HISTORY_REWRITE)`).  Each sever walks
the kept commits oldest first:
- **Parents:** cut parents are dropped, and rewritten ones replaced by
  their new hashes.
- **What stays:** root value, time and message, and the commit's own
  change bytes.
- **What is recounted:** generation and cumulative change bytes, so every
  measure the window takes from a head is as before.
- **Unchanged commits:** one with nothing cut beneath it keeps its hash.
- **What follows the new hashes:** branch heads, pending merge heads and
  keyframe images.  `vc_set_rewrite_hook` hears each old and new hash, so
  records outside the repo (`prolly.roots`, say) can follow.
- **Persistence and remotes:** the mode is saved with the repo.  Push,
  fetch, pull, clone and deepen return `VC_ERR_REWRITTEN`, since its
  hashes do not last.

**Tests** (`test/test_rewrite.c`):
- **init <- A <- B <- C <- D with a window of two** ends as C' <- D': C'
  has no parents and generation 1, the log ends cleanly, both hold
  exactly the rows they did, nothing but the two commits is known, and no
  old commit is in the store.
- **The store** stays the window's size over 300 severs.
- **Branches** over shared history get one new root and still merge, and
  a merge commit drops only its cut parent.
- **A sever with nothing to cut** changes nothing.
- **What follows the commits:** images and merge heads follow them.
- **The window:** measures from the head are unchanged, and the
  incremental window stays exact.
- **The mode** survives a reopen, and remotes are refused.
- **Each part of the rewrite was broken in turn, and each break was
  caught.**

**Cost at 1M commits** (`bench/deepbench --rewrite --due --max-commits
100000`):
- commit plus amortized severing is 132 us, against 112 us in KEEP mode;
- each sever rewrites up to the window's 100k commits: 426 ms, against
  196 ms;
- saves, reopens, logs, short merge bases and diffs are as in KEEP mode.

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

Superseded in part by the seam spike: see *Earlier results under the seam*
in `ARCHITECTURE.md` for what each result here means now, and its list of
what is left to build.  The order as it stood before the spike:

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
