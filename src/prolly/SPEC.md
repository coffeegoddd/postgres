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

- **The working set.**  Each branch of a versioned schema has its own heap
  tables and indexes, and its own rows in the system catalogs.  A session
  chooses which branch it works on, and sees that branch's tables under
  their usual names.  Unversioned schemas sit beside versioned ones and are
  shared by every branch.
- **The history store.**  Prolly trees and commits are stored as chunks
  inside Postgres, in pages of their own layout that go through Postgres's
  buffer manager and WAL.  History is therefore backed up, replicated and
  recovered with the rest of the database.
- **Capture.**  Logical decoding reads each branch's committed changes from
  the WAL and applies them to that branch's working tree in the history
  store.  A Dolt commit records the tree as of a point in the WAL, without
  blocking writers.
- **The window.**  The history kept locally is bounded by commits, size or
  time.  History that falls outside it is moved to an archive or a remote,
  or discarded.

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
- **No user code during version control.**  Checkouts, resets and merges
  reproduce rows that commits already hold, without running the user's
  triggers or rules a second time.
