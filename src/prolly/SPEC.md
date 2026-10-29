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
- **Capture.**  Logical decoding reads each branch's committed changes from
  the WAL and applies them to that branch's working tree in the history
  store.  A Dolt commit records the tree as of a point in the WAL, without
  blocking writers.
- **The window.**  A configurable stretch of history in which
  version-control operations are guaranteed to be efficient, at the cost of
  the storage it takes.  Its bounds are set by commits, size or time, or a
  combination of them.  History that falls outside it is moved to an
  archive or a remote, or discarded.

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
2. **Capture reads the changes back from the WAL.**  A background worker
   per branch decodes each committed transaction into row changes and
   applies them, in batches, to that branch's working tree in the history
   store.  Writers never wait for it.
3. **A Dolt commit marks a point in the WAL.**  Committing writes a marker
   into the WAL and waits until capture reaches it.  The working tree then
   holds exactly the transactions committed before the marker, and the
   commit records that tree.  Writers keep going while this happens.
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
  until vacuum reclaims them, and keyframes, when kept, add saved pages.

Version-control operations that cross the seam are therefore the ones
this design must make cheap.

- **Reading history needs only the history store.**  Log, diff, blame and
  history queries walk prolly trees, so they cost what changed, not the
  size of the tables.
- **Switching branches moves no data.**  Each branch has its own tables, so
  a session switches by choosing another branch.
- **Moving a branch to another commit applies the difference.**  Reset,
  checkout of an older commit and the result of a merge are computed in the
  history store, and then applied to the branch's tables as ordinary row
  changes.  When many rows differ, restoring a saved page image (a
  keyframe) and applying only the rest is cheaper, and a planner chooses
  between the two from measured costs.
- **A new branch shares its parent's pages.**  Its tables share pages with
  the parent copy-on-write, so creating it copies no data.
- **Rebuilding.**  A branch can be rebuilt from any commit's history alone,
  which is how clones and history outside the window come back to life.
  It crosses the seam with every row, so it is the costliest path.

### How the version-control features work

#### Branching

**What a branch is made of.**  A branch *B* of a versioned schema consists
of five things:
- **A head pointer**: a row in the history store mapping *B* to the hash of
  its latest commit.
- **A working-root pointer**: a row mapping *B* to the hash of its working
  tree, the prolly tree that capture keeps equal to *B*'s tables.
- **Catalog rows**: a `pg_namespace` row for the schema, marked as *B*'s,
  and under it *B*'s own rows in `pg_class`, `pg_attribute`, `pg_index`,
  `pg_constraint`, `pg_type` and the other catalogs, each with its own
  object IDs.  Objects in unversioned schemas have no per-branch rows;
  every branch uses the shared ones.
- **Block maps**: for each of *B*'s tables and indexes, a map from block
  number to the physical page that holds it.  A map is a tree whose nodes
  carry reference counts, so two branches can share a whole map, or any
  part of one, by pointing to the same node.  A page is shared when more
  than one branch's map reaches it.
- **A capture position**: the WAL position up to which *B*'s changes have
  been applied to its working tree.

**Reading and writing pages.**  Reading block *k* of a table on *B* looks
up *k* in *B*'s block map and reads that page through the buffer manager.
Writing to it first checks whether the page is shared.  If it is, the
write allocates a new physical page, copies the 8 KB into it, and points
*B*'s map entry at the copy, first giving *B* its own copy of any shared
map node on the way to that entry; then it writes the change to the copy.
The remapping is WAL-logged with the change.  Every
other branch keeps pointing at the old page.  Only that one page is ever
copied, and only on the first write to it.

**Working on a branch.**  A session sets its branch.  Its cached catalog
entries are discarded, and from then on table names resolve to *B*'s
catalog rows, its queries of the catalogs (`\d`, `information_schema`,
`pg_dump`) see only *B*'s definitions, and its reads and writes go through
*B*'s block maps.  Nothing else changes, so switching costs the same
whatever the size of the data.

**Creating a branch from HEAD, with a clean working set.**  The parent *P*
is clean when its tables hold exactly its head commit *H*: capture has
applied every change, and *P*'s working root equals *H*'s tree.
1. **Fix a point.**  Writes to *P*'s versioned tables are paused for a
   moment, so that every block map is read as of the same instant.
2. **Pointers.**  *B*'s head pointer is set to *H*, and its working-root
   pointer to *H*'s tree.  Both are new rows; no tree is copied.
3. **Catalog rows.**  Every catalog row of *P*'s versioned schema is copied
   under *B*, with new object IDs, and references between them rewritten
   to the new IDs.  The table statistics are copied with them.
4. **Block maps.**  Each of *B*'s tables and indexes gets a block map that
   is *P*'s: *B* points to the root of *P*'s map, and the root's reference
   count rises by one.  Nothing below the root is touched, and no page is
   copied.
5. **Capture position.**  *B*'s capture starts at the WAL position of
   step 1.  Writes to *P* resume.

From then on, the first write on either branch to a shared page copies
that page, as above.

**Creating a branch from HEAD, with a dirty working set.**  *P*'s tables
also hold changes committed in SQL since *H*: its working root *W*
differs from *H*'s tree.  First, capture applies *P*'s changes up to now,
so that *W* matches *P*'s tables exactly; then steps 1 to 5 run as above,
except for the working-root pointer.  The uncommitted changes are never
copied: they sit in pages that both branches now share.  Which branch keeps
them follows Dolt's checkout rules; on a branch that must not have them,
they are undone as row changes, each write copying the page it touches on
that branch only.

**Creating a branch from an arbitrary commit *X*.**  *B*'s head and
working-root pointers are set to *X* and *X*'s tree.  The working set is
then built from the cheapest source at hand:
- **A keyframe of *X*.**  A keyframe is a saved set of block maps, one per
  table and index, as of a commit.  *B*'s block maps are set to share the
  keyframe's, exactly as in step 4, and *B*'s catalog rows are made from
  *X*'s definitions.  No row is written.
- **A state close to *X*.**  The block maps of a branch, or of a keyframe,
  whose tables differ from *X* in few rows are shared as in step 4.  The
  definitions are then brought to *X*'s by DDL, and the rows that differ
  are rewritten on *B* as row changes, each write copying the page it
  touches.
- **Neither.**  *B*'s tables are created empty from *X*'s definitions and
  loaded with *X*'s rows.  Every row is written.

**Committing on a branch.**  A marker is written into the WAL, capture
applies *B*'s changes up to it, and a commit chunk is written that names
*B*'s working tree and *B*'s previous head.  *B*'s head pointer moves to
the new commit in the same transaction.  No page of the working set is
read or written.

**Deleting a branch.**  *B*'s catalog rows are dropped, and its block maps
are released: each map's root loses a reference, and map nodes and pages
that no branch's map reaches any more are freed, in the background.  *B*'s head and working-root pointers are
removed; history no other branch reaches is reclaimed by garbage
collection.

**Limits.**  Names in the history store are cheap: the prototype creates a
branch in 2 to 8 microseconds, flat up to a million branches.  What a
branch costs beyond that is its working set:
- **Storage.**  A branch costs only the pages it has changed.
- **Catalog size.**  Each branch adds a copy of the schema's catalog rows,
  and every object of every branch takes an object ID from a single 32-bit
  counter shared by the whole cluster.  A schema of 1,000 objects therefore
  allows on the order of millions of branches before IDs run short.
- **Catalog filtering.**  Each session filters out other branches' catalog
  rows, at a cost that grows with the number of objects on other branches
  (about 2 ms to rebuild after a schema change, at 12,000 hidden objects).

**Time complexity**, with *C* the number of objects in the versioned
schema, *d* the number of rows that differ and *n* the number of rows in a
table:

| Operation | Dolt | This design |
|---|---|---|
| Create from HEAD, clean | O(1) | O(C) |
| Create from HEAD, dirty | O(1) | O(C + d) |
| Create from an arbitrary commit | O(1) | O(C + d), or O(C) from a keyframe of that commit |
| Switch a session's branch | O(1) | O(1) |
| Commit on a branch | O(d log n) | O(d log n) |
| Delete a branch | O(1) | O(C) |

Dolt's creates cost O(1) because a Dolt branch is only a name: its working
set is a pointer to a tree, shared until written.  Ours cost O(C) because a
Postgres branch also needs its own catalog rows, which is what lets every
Postgres feature work on it unchanged; the data itself is shared, as in
Dolt.

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
