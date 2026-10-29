# Architecture

The layers the prolly engine is built from, bottom to top: what each owns,
its interface, what the prototype has, and what is missing.  The rule
throughout: every layer sits behind an interface, and a layer talks only to
the ones below it, or to the side through a hook.  `LIFECYCLE.md` holds the
measurements and decisions; this file is the map.

```
 9  SQL surface            functions, views, configuration
 8  Operations             the verbs: commit, checkout, merge, diff, sever
 7  Keyframes              physical cache: images, planner, manager
 6  Apply                  tree -> Postgres: moves, replay, restore, rebuild
 5  Capture                Postgres -> tree: stream decoder, barrier, DDL
 4  Working-set model      schema <-> tree: layouts, tags, codec, diff, merge
 3  Version control core   commits, branches, window, segments, remotes
 2  Prolly tree            ordered maps, diff, merge, value trees
 1  Chunk store            content-addressed chunks, refs, durability
```

Layers 1 to 3 know nothing of Postgres.  Layers 5 and 6 are the only ones
that talk to the server.  Layer 4 is the contract between the two halves:
how a schema's contents look as trees.

## The layers

### 1. Chunk store (`chunkstore.h`)

- **Owns:** chunks by hash; refs, pins and roots; orphan collection;
  compaction; the durable state root.
- **Interface:** `ChunkStore` with ops; memory and directory stores.
- **Status:** built and tested.
- **Open:** durability through Postgres's WAL, so the store's root moves
  with a commit record (see the decisions below).

### 2. Prolly tree (`prolly.h`, `chunkfmt.h`)

- **Owns:** ordered maps; the chunking policy; diff and three-way merge;
  value trees; trees that can be severed and repaired.
- **Status:** built and tested; asymptotic costs checked by chunk counts.

### 3. Version control core (`vc.h`)

- **Owns:** commits (with generation, change bytes and time), branches,
  working roots, merge base and log; the window (floors, ceilings, low
  marks), severing into segments, export and discard; history modes KEEP
  and REWRITE; remotes; the saved repo state.
- **Keyframes here are only holds:** a commit in the window may hold an
  image; a sever releases it; REWRITE moves it.  vc never decides which
  commits get one.
- **Status:** built and tested, flat to 1M commits and 1M branches.

### 4. Working-set model (`bench/ws_codec.c`, `ws_diff.c`, `ws_merge.c`, `ws_tags.c`, `ws_viol.c`)

- **Owns:** how a versioned schema maps into trees: table layouts (stored,
  generated, virtual columns), column tags, the row codec, schema objects,
  sequences, stored constraint violations; schema-aware diff and merge,
  including the evaluators a merge needs (`WsEval`).
- **Status:** works, but lives in the harness.  It must become a library
  behind an interface: layers 5 to 8 all depend on it.

### 5. Capture: Postgres to tree (`bench/ws_stream.c`, `ws_logical.c`, `pgoutput.c`)

- **Owns:** the streaming decoder on a logical slot; the barrier that makes
  a commit exactly "every transaction before it"; DDL carried in the
  stream by an event trigger; COPY capture as the slow reference.
- **Status:** works in the harness, as a libpq client.
- **Missing:** a home inside the server (see the decisions).

### 6. Apply: tree to Postgres (`bench/ws_move.c`, `contrib/prolly_pages`, `bench/prolly_tags.sql`)

- **Owns:** moving the working set to a commit: schema objects, rows by
  replay, sequences, the objects that need rows; full rebuild; page
  restore; the rule that version control runs no user code (row apply
  below triggers and rules, `prolly.quiet()`, copied generated values).
- **Status:** the C pieces are a real extension; the orchestration is
  harness code.

### 7. Keyframes (`image.h`, `kfplan.h`, `prolly_get_pages_frozen`)

- **Owns:** commit images (frozen pages in a page map, shared between
  images); the planner that prices replay against restore and learns its
  costs.
- **Missing: the manager.**  It decides when to capture, keeps the images
  within a budget inside the window, sweeps images of another format, and
  orders an image's durability against the state that names it.  It talks
  to layer 3 only through holds, restores through layer 6, and captures
  under layer 5's snapshot.

### 8. Operations (`bench/rebuild.c`, `bench/winbench.c` today)

- **Owns:** the verbs, each a sequence over the layers below.  A checkout
  is barrier (5), plan (7), move (6), advance the working root (3).  Also
  when to sever, and backups.
- **Status:** harness programs only.  This is why the prototype has felt
  scattered: each piece was wired into a harness, not into this layer.

### 9. SQL surface

- **Owns:** the functions users call (commit, checkout, branch, merge,
  reset), the system views (log, diff, status, conflicts), configuration
  (window policy, history mode, keyframe budget), permissions.
- **Status:** not started.

## Across the layers

- **Durability and crash order:** layers 1, 3 and 7.  What is durable is
  what a saved state names; everything else is collected.
- **No user code:** layers 6 and 8; merge's generated columns are the one
  documented exception.
- **Security, grants, branch permissions:** to prototype.
- **Concurrent branches:** copy-on-write working sets would replace the
  working-set half of layers 5 and 6.  Layers 1 to 4 and 7 to 9 stay.

## The seam: what stock Postgres gives

The engine goes inside Postgres as an extension below the table access
method seam, as OrioleDB does, so upstream changes keep merging.  Where a
seam cannot give a feature its full semantics, we patch: small,
hook-shaped patches, listed here with the feature that needs each.

### Experiment A: a branch as relation files (`contrib/prolly_branch`)

A branch of a schema is one set of files: a copy of every table, index,
TOAST relation and sequence file.  Checkout points `pg_class` at the
branch's files (with the fields that belong to a file: relpages,
reltuples, relallvisible, relfrozenxid, relminmxid), in a transaction.
No core change.  `spike/branch_test.sql`, `spike/sessions.sh` and
`spike/horizon_ddl.sh` test it.

**What works on stock Postgres:**
- every index method (B-tree, hash, GiST, GIN, SP-GiST, BRIN, pg_trgm's
  GIN) answers each branch exactly, with amcheck clean, since a table and
  its indexes switch together; TOAST and identity sequences are per branch;
- a checkout is transactional: rolled back, it leaves the branch it found;
  a branch made in a rolled-back transaction leaves no files;
- readers wait for a checkout and then see the new branch; prepared plans
  follow it; an open cursor holds a checkout off until its transaction
  ends;
- VACUUM and ANALYZE on one branch leave the others as they were;
- crash recovery: both branches survive an immediate stop, since every
  write to a branch's files is ordinary WAL.

**What it cannot give:**
1. **One branch per database at a time.**  `pg_class` names one file per
   relation, so every session sees the checked-out branch.  A branch per
   session needs the relation-to-file mapping to be per backend (experiment
   B).
2. **The same definition on every branch.**  Shown: after `ALTER COLUMN a
   TYPE bigint` on one branch, the other branch's file read under the new
   definition is garbage (`20/339457` for `2/20`).  Branches that differ in
   definitions need per-branch catalogs, or a checkout that moves the
   definitions (as the harness's moves do) and switches files only where
   the physical layout matches.
3. **Logical decoding does not see a switch.**  Writes on each branch are
   decoded, but the switch itself is not in the stream, so a subscriber
   merges both branches' rows into one table.
4. **Files the catalog does not name are invisible to the server's own
   bookkeeping.**  The freeze horizon (`vac_update_datfrozenxid`) and so
   commit-status truncation consider only `pg_class`: a parked branch's
   unfrozen xids can outlive the status the server keeps for them.  Parked
   files must be frozen when parked, or the horizon must include them (a
   hook).  Autovacuum likewise never visits them.
5. **DROP TABLE leaves the other branches' files** on disk.  Fixable from
   an extension with `object_access_hook`.

### Experiment B: a branch per session (`relation_locator_hook`)

A 14-line core hook at the end of `RelationInitPhysicalAddr` lets an
extension set a relation's file number for its own backend.
`prolly_branch.use_branch` sets a session GUC mapping each relation of the
schema to a branch's files and rebuilds those relcache entries.
`spike/session_branch_test.sh` tests it.

**What works with the hook:**
- two sessions on two branches at once, each reading and writing its own,
  every index exact and amcheck clean; a new session sees the checked-out
  branch;
- parallel workers read the session's branch (the GUC travels with them);
- a global checkout leaves a session that chose its branch on it;
- crash recovery with both branches written.

**What it breaks or still needs:**
1. **Logical decoding fails outright** on a session's branch files:
   `could not map filenumber ... to relation OID`.  Decoding maps files to
   relations through `pg_class`.  Needs a hook there (a patch), or our own
   WAL records decoded by our own resource manager.
2. **Per-file catalog fields are shared.**  VACUUM in a session on g wrote
   g's relpages and reltuples into `pg_class` (0/-1 to 66/2469, while main
   has 2375 rows); relfrozenxid goes the same way, so the freeze horizon
   is wrong for every other branch.  Needs those fields per branch, with
   hooks where VACUUM, ANALYZE and the horizon read and write them
   (patches).
3. **Sequence caches outlive a branch change:** a session that drew 21
   from g's sequence then drew 22 on main, from g's cache.  Fixable in the
   extension: reset the caches on a switch (`ResetSequenceCaches`).
4. **DDL lands in the shared catalog.**  An index built in g's session is
   built from g's rows; main's session then reads it and fails (`could not
   read blocks 65..65`).  With definitions moved at checkout (decision
   (b)), DDL must not run while sessions are on other branches of the
   schema: it takes the schema exclusively, or it is refused.
5. **Statistics are per relation, not per branch:** `pg_statistic` and the
   cumulative stats mix branches.  Row locks and SSI predicate locks are
   tagged by relation and TID, so two branches can contend falsely (not
   tested).
6. Everything experiment A could not give, except a branch per session.

### Catalogs: versioned at the row level (decided)

Versioned and unversioned schemas live side by side in one database
(`CREATE SCHEMA ... VERSIONED`), so catalogs branch by row, not by file:
- **one set of catalog files.**  Rows of objects in a versioned schema
  carry a branch; all other rows carry none and are shared.  A backend
  sees the shared rows plus its own branch's; unique indexes include the
  branch; a branch's copy of an object keeps its object id;
- **dependencies** point only from versioned objects to shared ones;
- **three classes of catalog data:**
  - versioned, committed to history: names and definitions (`pg_class`,
    `pg_attribute`, `pg_type`, `pg_proc`, `pg_constraint`, `pg_index`,
    `pg_trigger`, `pg_rewrite`, `pg_policy`, `pg_namespace`,
    `pg_sequence`, `pg_depend`, `pg_description`, ...);
  - per branch, never committed: `pg_statistic` and the physical columns
    of `pg_class` (relfilenode, relpages, reltuples, relallvisible,
    relfrozenxid, relminmxid);
  - shared, cluster-wide: `pg_authid`, `pg_auth_members`, `pg_database`,
    `pg_tablespace`, `pg_shdepend`, `pg_db_role_setting`,
    `pg_subscription`, ...
- **what it subsumes:** each branch's `pg_class` row names its own files,
  so experiment A's catalog swap and experiment B's locator hook become
  unnecessary; per-branch statistics and freeze ids come with the rows.
- **commits** hold the canonical form (definitions as DDL, column tags),
  not raw catalog rows, which name cluster-local ids; diff and merge work
  there, and the DDL moves bridge history and the physical catalogs.
- **the cost,** as measured by experiment C: a branch column and one index
  change on `pg_namespace`, branch-aware name lookups, and one filter in
  the visibility check for queries (not for lookups by object id): about
  520 lines in 14 files.
- Whole-database branching with a catalog file set per branch was
  considered and set aside: it cannot keep unversioned schemas shared.

### Experiment C: catalogs branched by row (`spike/catalog_branch.patch`)

A versioned schema has one `pg_namespace` row per branch, all with the same
name; `nspbranch` says whose row it is (0: shared).  Each branch's objects
are its own rows in every other catalog, made in its namespace under new
object ids: only `pg_namespace` gets a column, and its name index becomes
`(nspname, nspbranch)`.  A session's `catalog_branch` (a GUC) says which
branch it is on:
- **names** resolve to the session's branch's namespace, then to a shared
  one (`SearchNamespaceByName`; the five name lookups in core use it);
- **queries** do not see other branches' catalog rows: in `pg_namespace`,
  in every catalog with a namespace column (`pg_class`, `pg_type`,
  `pg_proc`, `pg_constraint`, operators, collations, text search, extended
  statistics), and in the catalogs describing those objects
  (`pg_attribute`, `pg_index`, `pg_attrdef`, `pg_trigger`, `pg_rewrite`,
  `pg_policy`, `pg_inherits`, `pg_sequence`, `pg_statistic`, `pg_enum`,
  `pg_range`, `pg_aggregate`, `pg_depend`, `pg_description`, ...);
- **lookups by object id are not filtered:** catalog snapshots are marked,
  and the filter leaves their reads whole, so decoding, `DROP ROLE` and
  dependency walks find every branch's objects;
- **the filter** is one test in the tuple visibility check (and the
  page-at-a-time scan), against a per-backend set of hidden object ids:
  other branches' namespaces and the relations, types, functions and
  constraints in them.  The set is rebuilt when a scan of a filtered
  catalog begins (never mid-scan, under a buffer lock), after a cache
  invalidation of any of those objects or a branch change;
- **logical decoding** on a branch carries the shared schemas' changes and
  that branch's; on no branch, all of them;
- changing branch resets the catalog caches and the search path.

The patch is about 520 lines in 14 files, kept as a patch file because it
changes the catalog (a cluster made without it cannot run with it).
Branch 2 was made by dumping branch 1's schema and restoring it on branch
2: the canonical-form path commits will use.  `spike/catalog_branch_test.sh`
makes its own cluster.

**What works:**
- each branch has its own definitions and rows (a column and an enum value
  added on one branch only); `information_schema`, `\d`, `pg_indexes` and
  `pg_dump` answer per branch;
- DDL on two branches at once, including the same table name on both;
- a shared schema beside it: writes from either branch seen by both, and a
  foreign key from a versioned table into a shared one;
- a session on no branch sees no versioned schema; `SET catalog_branch`
  inside a session, prepared statements included;
- each branch's table has its own `pg_class` row, so its own files,
  relpages, reltuples and relfrozenxid: experiment A's catalog swap,
  experiment B's locator hook and B's shared-numbers problem are gone;
- the freeze horizon is the minimum over every branch's tables;
  autovacuum visits every branch's tables (all are in the one
  `pg_class`), each with its own statistics;
- `DROP ROLE` is refused from any branch while another branch has objects
  the role owns, and names them;
- temporary tables work on a branch, and may share a versioned table's
  name; an extension in a shared schema serves every branch;
- dropping a branch's schema leaves the other branch's.

**What it does not give yet:**
1. **Extensions in a versioned schema exist on one branch only.**
   `pg_extension` is one row per name per database: on branch 2,
   `CREATE EXTENSION pg_trgm SCHEMA app` fails (`already exists`) while
   branch 1's copy is invisible to it.  Per-branch extensions need
   `pg_extension` filtered by its namespace and its name index widened, as
   `pg_namespace`'s was.
2. **The hidden set costs a catalog scan when stale.**  With 6,007 hidden
   relations (about 12,000 hidden objects), rebuilding it takes about 2 ms;
   any DDL anywhere makes it stale, so the next catalog query in each
   session pays that.  Indexes on the namespace columns, or keeping the
   set by the invalidations themselves, would make it proportional to what
   changed.
3. Shared catalogs naming branch objects (`pg_shdepend`,
   `pg_publication_rel`, `pg_subscription_rel`), security labels and
   initial privileges are not yet filtered; publications and subscriptions
   on versioned schemas are untried.

### Patches so far

| Patch | Feature | Status |
|---|---|---|
| `relation_locator_hook` (relcache.c, relcache.h) | a branch per session | spike |
| a relation-for-file hook in decoding | logical decoding of branch files | needed for B |
| per-branch file fields (relpages, reltuples, relallvisible, relfrozenxid, relminmxid) and the freeze horizon | VACUUM, ANALYZE, wraparound safety per branch | needed for B; A needs the horizon part |
| `pg_namespace.nspbranch`, branch-aware name lookup, the catalog filter, `catalog_branch`, branch-scoped decoding (`spike/catalog_branch.patch`) | catalogs branched by row | spike; makes the two above unnecessary |

## Order

1. This map.
2. Lift layers 4, 5, 6 and 8 out of `bench/` behind interfaces; the
   harness programs become thin drivers over them, and every existing
   check keeps passing.
3. The keyframe manager in layer 7.

## Decisions open

Listed with a recommendation each; until decided, the lift keeps both
options possible.

1. **Where layers 5 and 8 run:** inside the server (an extension: the
   decoder as a background worker, the verbs as backend functions) or
   outside it (a client process over libpq, as now).
2. **How layers 4 to 6 reach Postgres:** through an interface for running
   SQL and reading results, implemented over libpq and over SPI, so the
   same code runs in the harness and in the server.
3. **Where the chunk store lives:** files beside the cluster with their own
   fsync (as now), or under Postgres's WAL so the store's root and a
   commit record are one atomic step.
4. **Concurrent branches in the first build:** copy-on-write working sets
   now, or one working set per versioned schema first.
5. **Keyframe defaults:** off unless configured, captured in the
   background after a commit (not on the commit's path), within a byte
   budget.
