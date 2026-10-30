# Version-controlled Postgres: specification

## Postgres storage overview

Postgres's storage engine is made up of three parts: the heap, which
stores the rows of each table; indexes, which find rows in the heap and
are B-trees by default; and the write-ahead log (WAL), which provides
durability by recording every change before it is made.

The heap and indexes are stored on disk as files of 8 KB pages, and the
WAL is a sequence of append-only log files.

The following are the core features that Postgres's storage engine
enables:

- **Fast point reads and writes.**  Key lookups walk a shallow B-tree, and
  a commit waits only for one sequential WAL write.
- **High concurrency.**  Readers and writers do not block each other.
- **Cheap updates.**  An update that leaves indexed columns unchanged does
  not touch the indexes.
- **Data integrity.**  Keys, foreign keys and constraints are enforced
  inside the transaction that writes.
- **Rich indexing.**  Index types cover full-text search, JSON, arrays,
  geospatial data and very large tables.
- **Row-level security.**  Table policies limit which rows each user can
  see or change.
- **Row-level locking.**  A write locks only the rows it changes.
- **Serializable transactions.**  Concurrent transactions can be made to
  behave as if they ran one at a time.
- **Transactional DDL.**  Schema changes run inside transactions and can be
  rolled back.
- **Online schema changes.**  Indexes can be built without blocking writes.
- **Savepoints and two-phase commit.**  A transaction can roll back part of
  its work, or commit together with other systems.
- **Tunable durability.**  Each transaction can choose how long its commit
  waits for disks and standbys.
- **Partitioning.**  Large tables can be split into partitions that
  queries touch only as needed.
- **Extensions.**  New data types, functions and index types, such as
  PostGIS and pgvector, plug in without changing Postgres.
- **Durability and high availability.**  Crash recovery, standby servers
  and point-in-time recovery all replay the same WAL.
- **Change data capture.**  Committed row changes can be streamed out of
  the WAL to other databases and tools.

## Dolt storage overview

Dolt's storage engine, on the other hand, is made up of prolly trees,
commits and a chunk store.  Prolly trees store the rows and indexes of each
table; commits record each version of the database, as a root value that
reaches every table plus metadata such as the author, the time and the
commits before it; and the chunk store holds the prolly trees and commits
as immutable chunks.

Every node of a prolly tree and every commit is a chunk, named by a hash of
its contents.  For durability, Dolt also keeps a chunk journal, an
append-only log to which new chunks are written first.

The following are the unique core features that Dolt's storage engine
enables:

- **Branches.**  Creating a branch copies nothing: it is a new name for an
  existing commit.
- **Commits and history.**  Every commit is a complete, immutable version
  of the database, and every earlier version stays readable.
- **Diffs.**  Two versions compare in time proportional to what differs
  between them, not to the size of the database.
- **Merges.**  Branches merge three ways, row by row and cell by cell,
  including schema changes.
- **Conflicts as data.**  Rows that both sides changed differently are
  recorded as conflicts, which can be queried and resolved in SQL.
- **History in SQL.**  Any table can be read as of any commit, and the
  changes between commits can be queried like a table.
- **Reset, revert, cherry-pick and rebase.**  History can be rewound,
  undone, copied between branches and rewritten, as in Git.
- **Blame.**  Each row can be traced to the commit that last changed it.
- **Clone, push and pull.**  Databases exchange only the chunks the other
  side lacks, so copies stay in sync cheaply.
- **Storage proportional to change.**  Versions share every unchanged
  chunk, so keeping history costs only what changed.
- **Verifiable versions.**  A version's hash covers all of its contents,
  so two copies with the same hash are identical.

## Our hybrid approach

In order to meet the goal of getting Postgres's OLTP performance and full
feature set, without hard-forking it, and adding Dolt's full versioning
capabilities, I've come up with a hybrid storage approach.  This approach
aims to preserve four key invariants:

- The database is fully Postgres compatible: existing applications,
  drivers, tools and extensions work unchanged.
- If you never use the version-control operations, you always get stock
  Postgres.
- Versioned data can be used next to unversioned data.
- Versioning can be tuned to your specific use case.

This hybrid storage approach has four key parts:

- **The Postgres-style working set.**  Each branch of a versioned schema
  has its own heap tables and indexes, and its own rows in the system
  catalogs.  A session chooses which branch it works on, and sees that
  branch's tables under their usual names.  Unversioned schemas sit beside
  versioned ones and are shared by every branch.
- **The Dolt-style history store.**  Prolly trees and commits are stored as
  chunks inside Postgres, in pages of their own layout that go through
  Postgres's buffer manager and WAL.  History is therefore backed up,
  replicated and recovered with the rest of the database.
- **Capture.**  Capture is the process that copies each branch's changes
  from its tables into the history store.  Each branch has a working tree:
  a prolly tree in the history store that holds the same rows as the
  branch's tables.  Capture reads the branch's committed changes from the
  WAL with logical decoding.  Logical decoding is the Postgres feature
  behind logical replication, and it turns WAL records back into row
  changes: each insert, update and delete, with its table and column
  values.  It requires `wal_level = logical`, which adds to the WAL what
  decoding needs, such as the old row's key on an update or delete.  It
  emits a transaction's changes only once the transaction commits, one
  whole transaction at a time and in commit order, so aborted work never
  appears.  A replication slot records how far its reader has got, so
  the server keeps the WAL the reader has not reached yet.  Capture
  applies the changes to the working tree, so the tree follows the tables
  a short way behind.  A Dolt commit records the
  working tree as of a point in the WAL, without blocking writers.
- **The window.**  A configurable stretch of history in which
  version-control operations are guaranteed to be efficient, at the cost of
  the storage it takes.  Its bounds are set by commits, size or time, or a
  combination of them.  Operations at a commit outside the window still
  work, but pay the full cost of materializing that commit's prolly tree
  as the restored working set.  History can also be truncated to the
  window, in one of two ways.  Commits beyond it can be moved off the
  server, to an archive or a remote, and attached again later.  Or
  history can be rewritten, squashing everything beyond the window into a
  new root commit at its boundary.

The following are the core features this approach enables:

- **Everything Postgres offers, unchanged.**  SQL runs on ordinary tables,
  so performance, concurrency, integrity, row-level security, extensions,
  replication and backups work as they do in Postgres.
- **Everything Dolt offers, in SQL.**  Branches, commits, diffs, merges,
  conflicts as data, history queries, reset, revert, blame, and clone, push
  and pull, as Dolt has them.
- **Branches per session.**  Different sessions can work on different
  branches of the same schema at once, each with its own definitions and
  rows.
- **Versioned and unversioned data side by side.**  Only the schemas that
  need history pay for it.
- **History that replicates and backs up with the database.**  Standbys,
  base backups and point-in-time recovery cover history as well as the
  live tables.
- **Bounded history.**  The window keeps local history to a set size, while
  older history stays available from archives and remotes.
- **Upstream Postgres changes.**  Because the design builds on Postgres's
  storage instead of replacing it, new releases from postgres/postgres can
  be merged in, carrying only a small, isolated set of patches.

## Architecture

### How data flows in

1. **SQL writes go to Postgres as usual.**  An insert, update or delete
   changes the branch's heap pages and indexes and writes WAL, exactly as
   in stock Postgres.  Version control adds nothing to the statement or
   to its commit.
2. **Capture reads the changes back from the WAL.**  One background worker
   per database decodes each committed transaction once into row changes
   and applies them, in batches, to the working tree of the branch each
   change belongs to, in the history store.  Writers never wait for it.
3. **A Dolt commit marks a point in the WAL.**  Committing writes a marker
   into the WAL and waits until capture reaches it.  The working tree then
   holds exactly the transactions committed before the marker, in commit
   order, and the commit records that tree.  Writers keep going while this
   happens.
   - **The committing session's own changes.**  A Dolt commit made outside
     an explicit transaction first commits the session's work, so its
     changes come before the marker.  Inside an explicit transaction with
     uncommitted writes, a Dolt commit is refused, as Dolt refuses a merge
     over uncommitted writes.  Changes no transaction has committed never
     enter a commit.
   - **A crash** before capture reaches the marker loses only the Dolt
     commit, which never became visible.  The transactions it would have
     covered are in the WAL, and the next commit covers them.
   - **Lag and backpressure.**  Capture's lag, *L*, is reported.  The WAL
     it has not decoded is held for it by its replication slot, up to a
     limit (`max_slot_wal_keep_size`).  Before the limit, the server warns.
     Past it, the slot is invalidated, and the branch's working tree is
     rebuilt from its tables rather than from WAL that is gone.
4. **History is stored inside Postgres.**  The new tree nodes and the
   commit are written to the history store as chunks.  The branch's
   pointer to its new commit is updated in the same transaction, so the
   commit and its history become durable, replicated and recoverable
   together.

### How data flows back out

**The seam.**  The working set and the history store hold the same data in
two different forms: heap and index pages in Postgres, prolly trees in the
history store.  Every operation that moves data from one form to the other
crosses the seam between them, and crossing it is where this design pays
in time and space that neither Postgres nor Dolt pays alone:
- **Time.**  A row crossing into the working set is written as a heap
  tuple, an entry in every index on the table, and the WAL for each.  A row
  crossing into the history store is decoded from the WAL and written into
  a prolly tree.  Either way the cost grows with the number of rows that
  cross, multiplied by the work each row needs on the far side.
- **Space.**  A versioned table is stored twice, once in each form.  Rows
  rewritten into the working set also leave old tuple versions behind
  until vacuum reclaims them.

Version-control operations that cross the seam are therefore the ones
this design must make cheap.

- **Reading history needs only the history store.**  Log, diff, blame and
  history queries walk prolly trees, so they cost what changed, not the
  size of the tables.
- **Switching branches moves no data.**  Each branch has its own tables, so
  a session switches by choosing another branch.
- **Moving a branch to another commit applies the difference.**  A hard
  reset and the result of a merge are computed in the history store, and then applied to the branch's tables as ordinary row
  changes.
- **A new branch shares its parent's pages.**  Its tables share pages with
  the parent copy-on-write, so creating it copies no data.
- **Rebuilding.**  A branch can be rebuilt from any commit's history alone,
  which is how clones and history outside the window come back to life.
  It crosses the seam with every row, so it is the costliest path.

### How the version-control features work

#### Branching

Branches are copy-on-write.  Creating a branch copies no pages: it copies
the versioned schema's catalog rows and points each of the new branch's
tables and indexes at the parent's pages, which costs O(C), with *C* the
number of tables and indexes in the versioned schema.  A page is copied
only on the first write to it on either branch.  Branching from an
arbitrary commit starts the same way from HEAD, then rewrites on the new
branch the rows that differ between HEAD and that commit, which costs
O(C + D), with *D* the number of differing rows.
When HEAD is dirty, the uncommitted rows are undone on the branch that must
not keep them, which also costs O(C + D), with *D* the number of dirty rows.

**How shared pages stay isolated.**  The invariant is that no change to a
page shared between branches is seen by any branch but the one that made
it.  That covers every kind of change: a statement's, VACUUM's, pruning's
and freezing's, an index's (splits included), a truncation's, a sequence's,
and replay's during recovery or on a standby.  It is enforced in one place,
below every access method:
- **The storage manager maps a relation's logical blocks to physical
  ones,** through a block map per relation, and counts how many maps hold
  each physical block.
- **The buffer manager copies a shared page before it changes.**  Taking a
  page's exclusive content lock, which every change takes before its
  critical section, first copies the page to a block of its own and remaps
  it.  A hint bit, set under a lighter lock, is never written back to a
  shared page.
- **The copy is logged before the change that needs it,** so redo always
  finds the page at its own block, and redo itself never allocates or
  copies.
- **Each branch's indexes point into its own heap.**  Item pointers
  (ctids) name logical blocks, which both branches keep identical until
  either writes, so an index entry stays valid on each side.

Upstream's regression and isolation suites run with every table stored
this way.


**Time complexity**, with *n* the number of rows in a table:

| Operation | Dolt | This design |
|---|---|---|
| Create from HEAD, clean | O(1) | O(C) |
| Create from HEAD, dirty | O(1) | O(C + D) |
| Create from an arbitrary commit | O(1) | O(C + D) |
| Commit on a branch | O(D log n) | O(D log n) |
| Delete a branch | O(1) | O(C) |

Dolt's creates cost O(1) because a Dolt branch is only a name: its working
set is a pointer to a tree, shared until written.  Ours cost O(C) because a
Postgres branch also needs its own catalog rows, which is what lets every
Postgres feature work on it unchanged; the data itself is shared, as in
Dolt.

**Physical work.**  *D* counts rows, and is the cost the tables in this
section give.  The physical work behind a row change also includes:
- **P<sub>c</sub>, pages copied on first write:** at most one heap page
  for each row changed, and usually far fewer, since a page holds many
  rows.  A page is copied once per branch, so P<sub>c</sub> is bounded by
  the pages a branch has changed since it was created.
- **I, index work:** an entry in each index for each row changed, and the
  index pages those touch, which are copied the same way.
- **TOAST:** the out-of-line values a changed row holds, and their pages.

So a write to a branch costs what it costs in stock Postgres plus, once
per page, a copy.  The copy is the overhead to measure, in time and WAL,
against stock Postgres.

**Space complexity**, counting the storage an operation adds and keeps.
WAL is not counted, nor what an operation leaves for reclaiming: dead row
versions for VACUUM, and unreferenced chunks for garbage collection.

| Operation | Dolt | This design |
|---|---|---|
| Create from HEAD, clean | O(1) | O(C) |
| Create from HEAD, dirty | O(1) | O(C + D) |
| Create from an arbitrary commit | O(1) | O(C + D) |

A clean create adds the new branch's catalog rows and one block-map root
page for each of its tables and indexes, and shares every data page.  A
dirty create, or a create from an arbitrary commit, also rewrites *D* rows
on one branch.  Each page those rows are on is copied the first time it is
written, so the pages added are at most the pages the *D* rows are on,
plus their index entries.  Dolt adds only the branch's name.

**Materialized and unmaterialized branches.**  What makes our branches
cost more than Dolt's is the working set, not the branch itself.  So a
branch has one of two states, and moving between them is an explicit,
priced step:
- **Unmaterialized.**  The branch is only a head in the history store,
  as in Dolt: no catalog rows and no pages.  It can be read through
  history (log, diff, history and AS OF queries) but not written by SQL.
- **Materialized.**  The branch has a working set: its own catalog rows,
  and pages shared copy-on-write.  It can be written.

A branch is materialized when a session checks it out, when it is first
written, or on request.  It is dematerialized on request, or after a time
unused.  Dematerializing a branch whose working set holds uncommitted
changes is refused, as Dolt refuses a merge that would stomp them.  Its
head stays, and it gives back its catalog rows and pages.  Creating a
branch makes only its head; the costs in the tables above are paid when it
is materialized.

| Operation | Dolt | Every branch materialized | Materialized on use |
|---|---|---|---|
| Create a branch | O(1) | O(C), or O(C + D) | O(1) |
| Materialize at a materialized branch's HEAD | | | O(C) |
| Materialize at an arbitrary commit | | | O(C + D) |
| Dematerialize a clean branch | | | O(C) |
| Catalog filtering in checkout | | O(B × C) | O(B<sub>m</sub> × C) |

Here *B<sub>m</sub>* is the number of materialized branches.  A thousand
branches cost a thousand heads, and only those in use pay for a working
set.  Clones treat branches the same way: other branches arrive as heads,
and get a working set when checked out.

**Measured.**  On the prototype's branched catalogs, a versioned schema of
10 tables, each with a primary key and one index, and 300 branches.
Single runs.  This counts catalog work only: the prototype's catalogs have
no copy-on-write storage, so a branch's data was created rather than
shared.  Sharing it costs about 2 ms in production (see The window).

| Measure | All 300 materialized | 30 materialized | 2 materialized |
|---|---:|---:|---:|
| Checkout (switch, then the first query) | 1.92 ms | 0.46 ms | 0.31 ms |
| The same just after DDL elsewhere | 2.03 ms | 0.50 ms | 0.35 ms |
| Rows in `pg_class` | 15,423 | 1,023 | 463 |
| Materializing one more branch | 361 ms | 93 ms | 67 ms |

- Checkout's catalog filtering follows the materialized branches, not all
  of them.
- With every branch materialized, each new branch's catalog work grew
  with the branches before it: 98 ms each for the first 75, then 182, 260
  and 361 ms for each later 75.  Materialized on use, it costs about 70 to
  90 ms however many heads exist.
- The catalog holds 33× fewer rows, which everything that scans it, from
  autovacuum to `pg_dump`, benefits from.

#### Checkout

Checkout discards the session's cached catalog entries; from then on,
table names resolve to the checked-out branch's catalog rows, and reads and
writes go through that branch's pages.  No row is read or written, whether
the working set is clean or dirty: uncommitted changes stay in the pages of
the branch they were made on.

The one cost that grows is catalog filtering: the session rebuilds the set
of other branches' catalog rows it must hide, which costs O(B<sub>m</sub>
× C), with *B<sub>m</sub>* the number of materialized branches (about 2 ms
at 12,000 hidden objects in the
prototype).

As in Dolt, there is no checkout of a commit by itself (a detached HEAD):
a commit is checked out by creating a branch at it, as described in
Branching, or a branch is moved to it by a hard reset.

**Time complexity:**

| Operation | Dolt | This design |
|---|---|---|
| Check out from HEAD, clean | O(1) | O(1) in rows; O(B<sub>m</sub> × C) in catalog filtering |
| Check out from HEAD, dirty | O(1) | O(1) in rows; O(B<sub>m</sub> × C) in catalog filtering |
| Check out an unmaterialized branch | O(1) | O(C) to materialize it, then as above |

#### Diff

Diff compares two prolly trees in the history store, as Dolt does,
walking only the parts that differ.  This costs O(D log n), with *D* the
number of differing rows and *n* the number of rows in a table.

To diff the working set, against HEAD or an arbitrary commit, capture
first catches up: it decodes the branch's committed changes from the WAL
into the branch's working tree, up to the moment the diff starts.  This
costs O(L), with *L* the number of changes it had not yet applied.  The
diffing session's own uncommitted rows, which the WAL does not yet show,
are then read from its pages and merged into the diff.  Diffing one
arbitrary commit against another needs no catch-up, so it stays
O(D log n).

**Time complexity:**

| Operation | Dolt | This design |
|---|---|---|
| Diff working set and HEAD | O(D log n) | O(L + D log n) |
| Diff working set and an arbitrary commit | O(D log n) | O(L + D log n) |
| Diff an arbitrary commit and another | O(D log n) | O(D log n) |

Dolt needs no catch-up because its writes go straight into its trees.
Ours go into Postgres pages first, and capture follows them; *L* is how
far it trails, which stays small while capture keeps up.

#### Reset, soft

A soft reset moves only the branch's head pointer, to HEAD or to an
arbitrary commit, in one row update in the history store.  The branch's
tables and working tree are not touched, so whatever the working set
holds, clean or dirty, it keeps; it now differs from the new head by
whatever separates the two.  No row is read or written, and capture does
not need to catch up.  There is no staging area, so a commit always takes
the whole working set, and a soft reset to HEAD changes nothing.

**Time complexity:**

| Operation | Dolt | This design |
|---|---|---|
| Soft reset to HEAD, clean | O(1) | O(1) |
| Soft reset to HEAD, dirty | O(1) | O(1) |
| Soft reset to an arbitrary commit, clean | O(1) | O(1) |
| Soft reset to an arbitrary commit, dirty | O(1) | O(1) |

#### Reset, hard

A hard reset moves the branch's head pointer to the target, HEAD or an
arbitrary commit, and makes the working set equal to the target's tree,
discarding uncommitted changes.  Unlike the operations above, it crosses
the seam:
1. Capture catches up, as for a diff, so the working tree matches the
   tables, which costs O(L).
2. The working tree is diffed against the target's tree in the history
   store, which costs O(D log n), with *D* the number of differing rows.
3. Each differing row is written into the branch's tables as an ordinary
   insert, update or delete, with its index entries and WAL, which costs
   O(D).  Definitions that differ are changed by DDL first.
4. The working-root pointer is set to the target's tree directly, and
   capture skips the writes of step 3 instead of decoding them back.

When the working set is clean and the target is HEAD, *D* is zero and
nothing is written.

**Time complexity:**

| Operation | Dolt | This design |
|---|---|---|
| Hard reset to HEAD, clean | O(1) | O(1) |
| Hard reset to HEAD, dirty | O(1) | O(L + D log n), *D* the dirty rows |
| Hard reset to an arbitrary commit, clean | O(1) | O(L + D log n), *D* the rows that differ between HEAD and the commit |
| Hard reset to an arbitrary commit, dirty | O(1) | O(L + D log n), *D* the rows that differ between the working set and the commit |

Dolt resets in O(1) because it points the working set at the target's
tree.  Ours must rewrite the differing rows in the tables' pages, where
Postgres features read them.

**Space complexity**, counted as for branching:

| Operation | Dolt | This design |
|---|---|---|
| Hard reset to HEAD, clean | O(1) | O(1) |
| Hard reset to HEAD, dirty | O(1) | O(D) |
| Hard reset to an arbitrary commit, clean | O(1) | O(D) |
| Hard reset to an arbitrary commit, dirty | O(1) | O(D) |

Ours adds the *D* rewritten rows, as new row versions and index entries,
and copies any page still shared with another branch the first time it is
written.  The history store gains nothing, since the working root is set
to the target's tree, which it already holds.  Dolt only moves a pointer.

#### Merge

A merge of branch *T* into the current branch is computed in the history
store, as in Dolt, and then applied to the current branch's tables:
1. Capture catches up, so the working tree matches the tables, which costs
   O(L).
2. The merge base, the latest commit both branches share, is found by
   walking the commit graph, as in Dolt.
3. If the current branch's head is the merge base, the merge is a
   fast-forward: the result is *T*'s tree.  Otherwise the three trees are
   merged: the base is diffed against each side, and the two sets of
   changes are combined, which costs O((D<sub>1</sub> + D<sub>2</sub>) log
   n), with *D<sub>1</sub>* and *D<sub>2</sub>* the rows each side changed.
   As in Dolt, a merge is refused while the session's transaction has
   uncommitted writes, or while the working set has uncommitted changes
   in a table the merge would change; uncommitted changes in other tables
   are kept.
4. The rows that differ between the working tree and the result are
   written into the current branch's tables as ordinary row changes, and
   the working-root pointer is set to the result.  For a three-way merge,
   these are the *D<sub>2</sub>* rows that came from *T*.
5. Conflicts, rows both sides changed differently, are written as rows of
   conflict tables, as in Dolt, which costs O(K), with *K* the number of
   conflicts.  A merge without conflicts then commits, or for a
   fast-forward moves the head pointer to *T*'s head.

**Time complexity**, leaving out the search for the merge base, which is
the same in both:

| Operation | Dolt | This design |
|---|---|---|
| Fast-forward | O(1) | O(L + D log n), *D* the rows between the two heads |
| Three-way, no conflicts | O((D<sub>1</sub> + D<sub>2</sub>) log n) | O(L + (D<sub>1</sub> + D<sub>2</sub>) log n) |
| Three-way, with conflicts | O((D<sub>1</sub> + D<sub>2</sub>) log n + K) | O(L + (D<sub>1</sub> + D<sub>2</sub>) log n + K) |

A three-way merge costs about the same in both, since both compute it in
prolly trees and ours writes only the rows that came from *T*.  A
fast-forward costs more in ours, because Dolt moves a pointer where ours
must rewrite the rows *T* changed.

**Space complexity**, counted as for branching:

| Operation | Dolt | This design |
|---|---|---|
| Fast-forward | O(1) | O(D), *D* the rows between the two heads |
| Three-way, no conflicts | O(D<sub>2</sub> log n) | O(D<sub>2</sub> log n) |
| Three-way, with conflicts | O(D<sub>2</sub> log n + K) | O(D<sub>2</sub> log n + K) |

Both build the merged tree in prolly trees, adding the chunks on the paths
to the *D<sub>2</sub>* rows that came from *T*, and both store *K*
conflict rows.  Ours also writes those *D<sub>2</sub>* rows into the
current branch's tables, which adds O(D<sub>2</sub>) pages and index
entries, within the bound above.  A fast-forward adds no chunks to either
store; ours writes the *D* rows into the tables.

### The window

Without further adjustment, the hybrid model does not make
version-control operations at arbitrary commits scale.  Every operation
that moves a working set to another commit must rewrite the rows that
differ in the tables' pages.  *D*, the number of rows that differ between
the working set and the target, is at most the number of row changes made
by the commits between them, and at most the number of rows in both: it
tends to grow with the distance to the target, up to every row of the
tables.  Dolt does these in O(1), by pointing at another tree.  The
most severe gaps are:

| Operation | Dolt | Dustgres | At scale |
|---|---|---|---|
| Hard reset to an arbitrary commit | O(1) | O(L + D log n) | up to O(n log n) for a distant commit |
| Create a branch from an arbitrary commit | O(1) | O(C + D) | up to O(C + n) for a distant commit |
| Fast-forward merge | O(1) | O(L + D log n) | up to O(n log n) for a branch far ahead |

Operations that stay in the history store (diff, log, a three-way merge,
soft reset) or that only change pointers (checkout of a branch, creating a
branch from HEAD) are not affected.

The window is the answer to these gaps.  It is a stretch of recent
history, kept locally, in which version-control operations at arbitrary
commits are guaranteed to be efficient, at the cost of the storage it
takes.  It does so in two ways.  First, it bounds how much history is
kept: the history that falls outside the window can be truncated, archived
to a remote, or squashed into a new root at the window's boundary.
Second, within its bounds it keeps keyframes, snapshot-like images that
make moving a working set to a commit within the window cheap.

**How its bounds are set.**  The window is measured back from each branch
head, by number of commits, by the bytes of change they hold, by age, or by
a combination:
- **Ceilings** say how much history may be kept, for example 10,000
  commits or 50 GB.  A commit beyond any ceiling falls outside the window.
- **Floors** say how much must be kept, for example the last 100 commits
  or the last 7 days.  A ceiling wins over a floor.
- **Low marks** are optional, and choose how the window moves.  Without
  them it slides: each truncation cuts back to the ceilings, a little at a
  time.  With them it fills and empties: history grows up to the ceilings,
  is then cut back to the low marks in one step, and grows again.  This
  trades a steady size for fewer, larger truncations.

Heads, and commits that are pinned (by a tag, for example), are always
kept.  With no window set, every commit is kept, as in Dolt.

**Truncating history.**  Truncation is optional, and runs in the background
when history has grown about a tenth past the ceilings, so its cost per
commit does not grow with the window.  It works in one of two modes:
- **Keep hashes.**  The history cut off becomes a segment.  A segment can
  be exported to an archive or a remote, discarded, or attached again
  later.  The commits that remain keep their hashes and still name the
  parents they lost, so clones, pushes and pulls keep working across the
  cut.
- **Rewrite.**  The history cut off is let go at once, and the oldest
  commits kept are rewritten without parents, so they and their
  descendants get new hashes.  Nothing of the cut history remains, so this
  mode suits data that must not be retained; it rules out remotes, whose
  hashes would no longer match.

**Keyframes.**  The operations that move a working set to another commit,
hard reset and creating a branch from an arbitrary commit, cost O(D) in rewritten rows, with *D* the number of rows that differ
between the working set and the target.  A keyframe removes most of that
cost.  A keyframe of a commit *X* is a physical copy of the versioned
schema's working set as of *X*, made of four parts:
- **Pages.**  Every page of every relation in the schema (tables, indexes,
  TOAST tables and sequences, with their free space and visibility maps)
  as of *X*.
- **A page map.**  A prolly map from (relation, fork, block number) to the
  page stored there.  Pages are stored by hash, so keyframes share every
  page that did not change between them, and two keyframes can be diffed
  in time proportional to the pages that differ.
- **A relation map.**  A prolly map from each relation to its description:
  its kind, the table it belongs to, its size in blocks, and its catalog
  rows.
- **A header.**  It names *X*, the keyframe it was built on, the WAL
  position and MVCC snapshot of *X*, and the format it is valid for (the
  Postgres version, page size, catalog version and cluster).

**Keyframe pages are frozen.**  A keyframe is read under *X*'s snapshot:
every heap tuple that *X* sees is frozen, so that it names no transaction,
and every tuple *X* does not see is removed.  A raw page names transaction
IDs that the server forgets as time passes; a frozen page names none, so
it can be put back at any later time and shows exactly *X*'s rows.

**Restoring a keyframe.**  In production, under copy-on-write, a
keyframe's pages are kept as copy-on-write pages, and its page map is a set
of block maps.  Moving a working set to *X* then shares the keyframe's
block maps, as creating a branch does, and makes its catalog rows from the
relation map, which costs O(C).  In the prototype, keyframes are stored as
chunks in the history store, and restoring one writes back only the pages
that differ from the working set, which costs O(P), with *P* the number of
differing pages; *P* is at most *D*, and usually far smaller, since a page
holds many rows.  When the target has no keyframe but one lies near it, the
nearest keyframe is restored and only the rows between the two are
rewritten; a planner chooses, from measured costs, between that and
rewriting rows from the current state.
**When keyframes are taken.**  Keyframes are spaced by change, not by
commits: a new one is taken once the rows changed since the last one reach
a spacing *Δ*.  Every commit in the window then lies within about *Δ*/2
rows of a keyframe, so moving a working set to any commit in the window
costs at most a restore plus replaying *Δ*/2 rows, however many commits the
window holds.  *Δ* is set from three things:
- **The break-even point.**  Below a certain size of change, replaying rows
  is cheaper than restoring pages, so *Δ* never goes below it.  The planner
  learns this point from the measured costs of the moves it makes.
- **The storage budget.**  Each keyframe holds about the pages touched by
  its *Δ* rows, so all keyframes together hold about the pages touched by
  all the change in the window.  A smaller budget means a larger *Δ*.
- **The target cost.**  Optionally, a bound on the slowest move in the
  window, such as one second, which sets the largest *Δ* allowed.

How this plays out depends on the window:
- **A window with little change**, below the break-even point, has no
  keyframes: replaying rows is always cheaper, and the costs of the
  sections above apply.
- **A window with millions of commits** has one keyframe per *Δ* rows of
  change.  At 1,000 rows per commit and a *Δ* of 100,000 rows, that is one
  keyframe per 100 commits, or 10,000 for a million commits.  When the
  budget or the workload changes, dropping every other keyframe doubles
  *Δ*; since keyframes share pages, each drop frees only the pages no other
  keyframe holds.

Keyframes are taken by a background worker, after a commit, never on its
path.  Taking one copies, and freezes, each page changed since the keyframe
it is built on, so its cost follows the write rate, not the number of
commits; when the worker falls behind, it skips keyframes and the spacing
widens until it catches up.

**Keyframes live only inside the window.**  A keyframe is released when
its commit leaves the window.  Keyframes are physical and local: they
belong to this cluster, are not sent to remotes, and can always be rebuilt
from the prolly trees, which stay the portable record.

**Keyframes are never needed for correctness.**  A commit's prolly tree is
its authoritative state.  A keyframe is an acceleration structure only:
- **Any keyframe can be lost or dropped,** and every operation still works
  by replaying rows from the trees, only slower.
- **A keyframe that can't be trusted is not used.**  That covers one of
  another format (server version, page size, catalog version, cluster), or
  one whose relations no longer match the target's catalog rows.
- **A restore runs under apply's lock,** which keeps the branch's writers
  out.  It becomes visible only at its commit, after these checks:
  - the relations restored are exactly those the target's relation map
    names;
  - each is given new storage and catalog rows in the same transaction;
  - the working tree is set to the target's tree.
- **It is not MVCC-safe for snapshots older than it,** like `TRUNCATE`: a
  reader whose snapshot predates the restore gets an error, never wrong
  rows.
- **Every restore is tested against a replay to the same commit,** row by
  row and with amcheck on every index.

**The planner: replay or restore.**  Every move of a working set to
another commit, a hard reset or a branch created at an arbitrary commit,
has two ways there, and a planner chooses between them for each move:
- **Replay:** rewrite the *D* rows that differ between the working set and
  the target.
- **Restore:** restore a keyframe near the target, then replay the *D'*
  rows that differ between the keyframe and the target.  *D'* is zero when
  the target has a keyframe of its own.

The planner prices each from counts it has before writing anything:
- **Replay** costs a fixed part, a cost per row, and a cost per page the
  rows are on.  A page first written since the last checkpoint ("cold")
  costs more than one already written since ("warm"), because its first
  change logs a whole image of it.
- **Restore** costs a fixed part plus the restore's own work, then the
  replay of the *D'* rows after it.  In production the restore's own work
  is sharing block maps, O(C).  In the prototype it is writing back the
  pages that differ, O(P).
- **The candidates** are the keyframes nearest the target, before and
  after it.  Their distances come from each commit's running count of
  change, so finding them needs no diff.

The planner calibrates itself:
- It starts from costs measured for the server and its `wal_compression`.
- It refits them from the time every move actually takes, by least
  squares, with older moves weighing less.
- It learns only from the path it takes, so a path not taken for 50 moves
  is tried on the next small move.  That keeps a wrong first guess from
  hiding a path for good.

The same prices decide when a keyframe is worth taking: once restoring
the change since the last keyframe, plus taking the new one, would beat
replaying that change.

A plan must cost O(1).  The counts it needs are kept as capture decodes
the WAL, not computed by scanning it.  The prototype computed one by
scanning the WAL since the working set's base, and planning grew to 2.3 s
while the moves took 70 to 320 ms.

**What is prototyped.**  The planner is built and tested, with its
calibration.  The keyframe format, including frozen pages, page maps and
page diffs, is built and tested too.  Restoring by sharing block maps has
been measured on the production storage manager.  What remains to build
is the manager that acts on the planner: the background worker, the
spacing and budget, thinning, and releasing keyframes as the window moves.

**Measured: restoring a keyframe against replaying rows.**  Three
measurements, each checked by comparing every row with the target's
(checksums or row-by-row) and by amcheck on every index.

*1. The production restore, from held block maps.*  On the production
storage manager, a table and its two indexes were captured as held
block-map roots at a state X.  After D rows changed, the table was put
back exactly to X in one of two ways.  Replay rewrote the D rows from a
saved copy of X.  Restore made new storage from the held roots and pointed
the table's and indexes' catalog entries at it.  The target is always X
itself, so this is a restore to a keyframe of the target, with nothing to
replay after it.

| Table | Rows changed | Replay | Restore |
|---|---:|---:|---:|
| 1M rows | 100k | 1,345 ms | 2.1 ms |
| 1M rows | 1M | 9,038 ms | 2.0 ms |
| 10M rows | 100k | 4,576 ms | 2.6 ms |
| 10M rows | 1M | 16,657 ms | 1.6 ms |
| 10M rows | 10M | 124,176 ms | 4.4 ms |

The restore costs about 2 ms whatever the table's size and however much
changed: O(C), as stated above.  (The replays scanned both tables to find
the changed rows, which inflates replay most for small changes, so small
changes are left out of this table.)

*2. Real keyframes in the prototype, by distance.*  A table of 1M rows,
40 commits of 20,000 random row changes each, a keyframe at every commit
(not frozen), `wal_compression = lz4`.  Each checkout is to a random
commit, by replaying the rows that differ or by restoring that commit's
keyframe.  The prototype restores by writing back the pages that differ,
so its restores cost what those pages cost, not production's 2 ms.

| Commits back | Replay | Restore |
|---|---:|---:|
| 1 | 324 ms (19k rows) | 1,205–1,247 ms |
| 4 | 722–989 ms (74k rows) | 1,210–1,466 ms |
| 10 | 1,188 ms (180k rows) | 1,086–1,277 ms |
| 19 | 1,744–2,216 ms (313k rows) | 1,217 ms |
| 26 | 2,496 ms (418k rows) | 1,145 ms |
| 38 | 3,307 ms (574k rows) | 1,423 ms (37 back) |

- Replay grows with the rows that differ, about 6 µs a row.
- Restore is flat.  Random changes of this size touch about 19,000 of the
  table's 23,700 pages in one commit, so nearly every page differs, and
  the pages that differ can never exceed the table's.  This spread of
  changes is the worst case for a restore that writes pages.
- They cross at about 10 commits back.  At 38 the restore is 2.3× faster,
  and the gap widens with distance.
- The planner, choosing on the same history, replayed targets 1 to 3
  commits back and restored from about 9 back.  It learned that crossover
  from its own moves.

*3. The same, under each `wal_compression`.*  A restore logs every page
whole, so it depends on the setting; replay hardly does.  Medians in ms,
replay / restore:

| Commits back | off | pglz | lz4 | zstd |
|---|---|---|---|---|
| 1–3 | 694 / 1,403 | 727 / 2,313 | 634 / 1,248 | 637 / 1,367 |
| 10–19 | 2,130 / 1,495 | 1,701 / 2,662 | 1,787 / 1,311 | 2,008 / 1,375 |
| 30+ | 3,569 / 1,706 | 3,035 / 3,029 | 2,985 / 1,485 | 3,240 / 1,655 |

Under lz4, zstd and off, restore wins from about 10 commits back.  Under
pglz it never wins within 38.  So keyframes want lz4 (or zstd), and the
planner keeps its costs per setting.

*Not yet measured:*
- the production restore followed by a replay of *D'*;
- frozen keyframes taken in the background;
- how much storage copy-on-write keyframes hold for a given spacing.

The restore half costs about 2 ms in production, so a move's cost is
essentially the replay of *D'*, which spacing sets: more keyframes, smaller
*D'*, more storage.

**Time complexity with keyframes**, in production, with *C* the number of
tables and indexes in the versioned schema, and *D'* the number of rows
between the target and its nearest keyframe (in the prototype, each O(C)
of a restore is O(P) instead):

| Operation | Without a keyframe | With a keyframe of the target | With a keyframe near the target |
|---|---|---|---|
| Hard reset to an arbitrary commit | O(L + D log n) | O(L + C) | O(L + C + D' log n) |
| Create a branch from an arbitrary commit | O(C + D) | O(C) | O(C + D') |

**Space complexity in and outside the window**, counted as in the sections
above (the storage an operation adds and keeps).  A commit in the window
lies within about *Δ*/2 rows of a keyframe, so *D'* is at most about
*Δ*/2; a commit outside it has no keyframe, and is reached by rewriting
rows from the current state, as in the sections above.  HEAD, and both
heads of a merge, are always in the window.

| Operation | Outside the window | In the window |
|---|---|---|
| Create a branch from HEAD, clean | O(C) | O(C) |
| Create a branch from HEAD, dirty | O(C + D) | O(C + D) |
| Create a branch from an arbitrary commit | O(C + D) | O(C + D') |
| Hard reset to HEAD, clean | O(1) | O(1) |
| Hard reset to HEAD, dirty | O(D) | O(D) |
| Hard reset to an arbitrary commit, clean or dirty | O(D) | O(C + D') |
| Fast-forward merge | O(D) | O(D) |
| Three-way merge | O(D<sub>2</sub> log n + K) | O(D<sub>2</sub> log n + K) |

In the window, a branch or reset to an arbitrary commit shares its nearest
keyframe's block maps, one root per table and index, and rewrites only the
*D'* rows after it; a reset lets go of the pages the working set held.  So
the space it adds no longer grows with how far back the commit is.
Operations from HEAD, and merges, are the same in both: they never move to
another commit's state, and keyframes are not used to merge.  If a merge's
base was cut from local history, finding it first attaches the cut history
again, from an archive or a remote, which adds that history back.

Keyframes' own storage is a standing cost of the window, not of any
operation: about the pages touched by all the change in the window,
bounded by the storage budget that sets *Δ*.

**Proposed: exponential spacing, tiered history, and no hard window.**
To be prototyped before it is adopted.  With even spacing, cost has a
cliff at the window's edge.  Inside it, any commit is within *Δ*/2 rows
of a keyframe.  Outside it, there is no keyframe, and a move costs O(D).
Two changes would replace the cliff with a slope.

- **Exponential spacing.**  Keyframes would be dense near the heads and
  sparser with age.  Measured in rows of change back from the nearest
  head, they would sit at about *Δ<sub>0</sub>*, *Δ<sub>0</sub>r*,
  *Δ<sub>0</sub>r<sup>2</sup>*, and so on, with *r* > 1.  As the heads
  move on, a keyframe would be dropped once its neighbours are closer than
  the spacing at its distance, the way backups are thinned from hourly to
  daily to monthly.
  - A commit *d* rows back would then be within about
    *d*(*r* − 1)/(2*r*) rows of a keyframe.
  - Covering *H* rows of history would take about 1 +
    log<sub>*r*</sub>(*H* / *Δ<sub>0</sub>*) keyframes, against *H* / *Δ*
    for even spacing.
  - Each keyframe holds only the pages it does not share with its
    neighbours, at most a whole schema's pages.  So the total stays
    bounded without cutting history.
- **Tiered history.**  The window's edge would stop being where history
  ends, and become where it moves to cheaper storage.
  - Recent history and keyframes stay on the server.
  - Older history chunks, and older keyframes exported as portable chunks,
    would move to object storage or a remote, and be fetched when an
    operation needs them.  A keyframe's exported form would be its frozen
    pages, page map and relation map, as the prototype stores them.
  - History chunks are immutable and named by hash, so they can live
    anywhere; this is the keep-hashes mode's segment, attached on demand
    instead of by hand.
  - An exported keyframe is restored by writing its pages, O(P), not by
    sharing block maps, O(C).  That still costs far less than replaying
    every row.

Truncation would stay for data that must not be kept (rewrite mode).

The proposed costs to reach a commit *d* rows of change back, with *r* the
spacing ratio and *P* the pages that differ:

| Operation | Even spacing, in the window | Even spacing, outside it | Exponential, on the server | Exponential, tiered off it |
|---|---|---|---|---|
| Hard reset | O(L + C + Δ log n) | O(L + D log n) | O(L + C + d(r − 1) log n) | O(L + P + d(r − 1) log n), plus fetching |
| Create a branch | O(C + Δ) | O(C + D) | O(C + d(r − 1)) | O(P + d(r − 1)), plus fetching |
| Keyframes kept | *H*/*Δ* in the window | | log<sub>*r*</sub>(*H*/*Δ<sub>0</sub>*) | |

Recent commits would stay as cheap as today.  Old ones would cost more
with distance, since the rows replayed after the nearest keyframe grow
with it.  Once that gap covers most of a table's turnover, a move costs
about what rewriting the table does.  Exponential spacing trades that cost
far back for bounded storage.  Nothing is lost unless the data must not
be kept.

**What the prototype must show** before either is adopted:
- the planner's choices and measured move costs under exponential spacing,
  against even spacing, on `winbench` and `kfbench`, across history depths;
- the storage held by keyframes under each spacing;
- the cost of a move to tiered history: fetching chunks and an exported
  keyframe from a file remote, then restoring.

### Compared with stock Postgres: OLTP performance

- **Statements run on the stock path.**  Reads and writes use the same
  heap, indexes, buffer manager and WAL as stock Postgres, at the same
  speed.
- **The standing cost is capture.**  Logical decoding needs a little more
  WAL per change, and the capture worker uses CPU and writes the history
  store in the background.  This cost scales with the rate of change, not
  with the size of the database, and it applies only to versioned schemas.
- **Dolt commits are short.**  A commit waits only for capture to reach its
  marker: measured at about 71 to 74 ms on 1.8 GB and 17 GB tables alike,
  and 123 ms including 2,000 pending row changes.

**Measured with sysbench.**  Dolt's sysbench OLTP tests ran on one server,
first against a plain database (stock Postgres), then against the same
tables in a versioned schema with capture running beside them: every
change decoded from the WAL, and a Dolt commit every 5 seconds into the
history store inside Postgres.  One table of 10,000 rows (Dolt's standard
setting), 30 seconds per test, throughput in transactions per second:

| Test | Threads | Stock Postgres (tps) | Prototype with capture (tps) | Difference |
|---|---:|---:|---:|---:|
| oltp_point_select | 1 | 45,863 | 47,683 | +4.0% |
| oltp_read_only | 1 | 2,275 | 2,400 | +5.5% |
| select_random_points | 1 | 21,097 | 22,611 | +7.2% |
| select_random_ranges | 1 | 8,078 | 8,414 | +4.2% |
| oltp_read_write | 1 | 629 | 638 | +1.4% |
| oltp_write_only | 1 | 994 | 993 | -0.1% |
| oltp_insert | 1 | 1,177 | 1,128 | -4.2% |
| oltp_update_index | 1 | 1,010 | 995 | -1.5% |
| oltp_update_non_index | 1 | 1,278 | 1,271 | -0.6% |
| oltp_point_select | 16 | 344,628 | 331,332 | -3.9% |
| oltp_read_only | 16 | 25,060 | 24,275 | -3.1% |
| select_random_points | 16 | 141,458 | 137,294 | -2.9% |
| select_random_ranges | 16 | 58,958 | 57,114 | -3.1% |
| oltp_read_write | 16 | 6,589 | 6,255 | -5.1% |
| oltp_write_only | 16 | 9,896 | 8,667 | -12.4% |
| oltp_insert | 16 | 8,650 | 9,427 | +9.0% |
| oltp_update_index | 16 | 8,464 | 8,647 | +2.2% |
| oltp_update_non_index | 16 | 10,049 | 9,498 | -5.5% |

- **With one thread, capture is not measurable:** every difference is
  within run-to-run variation, in both directions.
- **With 16 threads, capture costs about 3 to 5% on most tests,** and 12%
  on write-only, the test that produces the most change to capture.
- **Capture itself took about 265 ms per 5-second interval,** about 5% of
  one core, plus 25 ms to write each commit's history.  At the end, the
  last capture matched the database read whole.
- **Limits of this run:** each test ran once, so differences under about
  5% are noise; the table fits in memory; and the prototype's capture runs
  as a separate client process that also records definitions with
  `pg_dump` at each commit, where the design carries them in the WAL
  stream.

### Compared with stock Dolt: version control without a window

With no window, every commit is kept locally, as in Dolt.

- **The same algorithms.**  Commits, diffs, merges, merge bases and logs
  use prolly trees as Dolt does, so their costs grow with what changed,
  as Dolt's do.  At the engine level, commits stay flat from 1,000 to
  1,000,000 commits held (about 110 microseconds each).
- **Branch switches are as cheap as Dolt's.**  Both only change which
  version a session reads.
- **Moving a branch costs more than in Dolt.**  Dolt only swaps a root;
  here the rows that differ are rewritten into the branch's tables:
  measured at 162 ms to reset across about 8,000 rows.
- **Creating a branch costs a little more than in Dolt.**  Both share the
  data; ours also makes the branch's own catalog rows, at a cost that grows
  with the number of objects in the schema, not with the data.
- **History lives in Postgres pages.**  Its reads go through the buffer
  manager, which is slower than Dolt's memory-mapped chunk files, but it
  is replicated and backed up with the database.
- **Not yet measured:** the same operations against Dolt itself.
