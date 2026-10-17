# 11. Pages for the working set, Dolt-style history

**Status: steps 0–5 built and merged (2026-10-17), with the gaps each
step's notes list; steps 6–8 not started.** This replaces the design in which prolly trees are a table's
primary storage (engines D1–D4, `04-candidate-designs.md`). The decisions
below marked "user" were made on the dates given; everything else is a
recommendation to be settled before the step that needs it. Part 4 is the
plan, with what each step built and where the build departed from it.

## 1. The design

### Two layers

- **Versioned schemas sit beside ordinary ones** (user, 2026-10-16). A
  database can hold both. Everything in a versioned schema is versioned:
  its tables, which are prolly tables (table access method `prolly`), and
  its views, functions, types, sequences and every other object. Ordinary
  schemas and their normal tables are untouched by any of this.
- **The working set is pages.** A prolly table, its indexes and its TOAST
  table are ordinary Postgres relations in 8 KB pages: heap's page format,
  nbtree, GIN, GiST and the rest, unchanged. Reads, writes, MVCC, row
  locks, SSI, HOT, TOAST, VACUUM, index-only scans and parallel scans are
  Postgres's own, so performance and concurrency are heap's.
- **Prolly pages are kept apart from normal pages** (user). Prolly
  relations live in block-mapped storage (part 5); normal relations stay
  in md.c's files, so prolly storage never interferes with normal
  storage.
- **History is a chunk store shaped like Dolt's.** Commits are immutable,
  content-addressed chunks of prolly trees, kept the way Dolt keeps them:
  new chunks appended to a journal, older ones in immutable table files
  with an index, and a manifest naming the root. The formats are our own
  (user: no Dolt or Doltgres byte compatibility); Dolt is the model for
  structure and behavior, not for bytes.
- **A Dolt commit snapshots the working set into the store**, and it
  captures every versioned schema in the database: every prolly table's
  rows and every versioned object's definition (user). Ordinary schemas
  are never in commits. A SQL commit
  is not a Dolt commit: SQL commits only change pages; `dolt_commit()`
  folds the branch's changes since its head into new trees and a new
  commit.
- **There is no staging area** (user). A Dolt commit takes the branch's
  whole working set (or the tables it names, without a persistent staged
  state).

Both extremes are valid and cheap: a table that has only the initial Dolt
commit and only ever changes in its working set is a heap table in all but
name and storage; a table where every SQL commit is also a Dolt commit keeps its delta
at one transaction's size.

### Versioned schemas and branches (user, 2026-10-16)

- **A versioned schema** is made by `CREATE SCHEMA … VERSIONED`. Every
  object in it is versioned per branch: tables, indexes, sequences,
  views, materialized views, composite and other types, domains, enums,
  functions, procedures, aggregates, operators, operator classes and
  families, collations, conversions, text search objects and extended
  statistics, with everything attached to them (constraints, defaults,
  triggers, rules, policies, privileges, comments). This is Doltgres's
  scope (its root value versions tables, types, functions, sequences,
  triggers and more), drawn at a schema instead of a database because
  ordinary schemas share the database.
- **Tables in a versioned schema are prolly tables**: `prolly` is their
  access method whatever the default, and another access method,
  `UNLOGGED` or moving a table into an ordinary schema is refused (a
  temporary table lives in `pg_temp`, outside). A prolly table can't be
  created outside a versioned schema; this replaces step 1's rule that
  any permanent table could be one.
- **Dependencies point one way.** A versioned object may depend on shared
  ones (an ordinary schema's table, type or function, an extension, a
  role, a cast, a language); a shared object may never depend on a
  versioned one, since it would point into one branch only. The rule is
  checked where Postgres records dependencies (`pg_depend`), so it covers
  foreign keys, column types, defaults, views, triggers and function
  bodies parsed at creation alike. Extensions, event triggers,
  publications, foreign data wrappers and other objects outside schemas
  stay shared; creating an extension inside a versioned schema is refused
  for now.
- **Each branch has its own copy of each versioned schema, with the same
  OIDs** (user, 2026-10-16). A branch's copy of an object keeps its
  source's OID; only its catalog rows' branch and, for a relation, its
  relation file number differ. Row data holds OIDs: a TOAST pointer names
  its TOAST table, an enum value is the OID of its label, an array or a
  composite value names its type, a `reg*` column names an object. Pages
  shared between branches carry them unchanged, so identical OIDs are
  what lets a branch read them as its own: a TOAST pointer finds the
  branch's TOAST table, an enum value the branch's label. New OIDs per
  copy would leave every such value pointing at the source branch's
  objects, and translating them where they are read would be a special
  case per kind of value.
- **Catalog rows carry a branch.** Every catalog that can hold a versioned
  object's rows (`pg_class`, `pg_attribute`, `pg_type`, `pg_proc`,
  `pg_namespace`, `pg_constraint`, `pg_index`, `pg_attrdef`, `pg_trigger`,
  `pg_rewrite`, `pg_policy`, `pg_enum`, `pg_sequence`, `pg_depend`,
  `pg_description`, `pg_statistic`, `pg_statistic_ext` and its data,
  `pg_inherits`, `pg_partitioned_table`, the operator, opclass, collation,
  conversion, aggregate and text search catalogs, and the rest found by
  the same rule) gains a branch column: zero for shared rows, the
  branch's id for a versioned object's. Each unique index gains the
  column, so a versioned object's rows can repeat its OID and name once
  per branch.
- **A backend sees one branch.** Catalog scans, by index or not, return
  shared rows and the session's branch's rows only. A backend's caches
  (relcache, catcache, typcache, plan cache) are keyed by OID as before:
  within one backend an OID names one row. Changing a session's branch
  resets them, as an invalidation overflow does. An invalidation from a
  backend on another branch costs a needless reload, nothing more.
- **New objects get new OIDs from the cluster's counter**, so an object
  created on one branch never collides with an object on another. A
  merge that brings an object from another branch keeps its OID when no
  object on the target branch has it, so copies stay identical along
  their lineage.
- **Names follow from the namespace.** A versioned schema keeps its OID on
  every branch too, so qualified names, `search_path`, `regclass` output,
  error messages, `\d` and `pg_dump` print and resolve the names users
  wrote, and two branches' objects of the same name differ only in
  branch.
- **Creating or dropping a versioned schema, and DDL in one, happens on
  the session's branch only**, so branches diverge in schema and merge,
  as in Dolt.
- **Copies share pages copy-on-write.** Under each prolly relation's
  files is a **block map** from logical block to physical block in a page
  store shared by the database's prolly relations. A branch's copy of a
  table shares its source's pages: no page is copied when the branch is
  made, and the first logical change to a shared page on either branch
  gives that branch its own copy of the page.
- **Making a branch's copies** copies its source's catalog rows with the
  new branch's id and new relation file numbers, and attaches each
  prolly relation's pages copy-on-write. Nothing inside the rows changes:
  with the same OIDs, defaults, view and rule trees, function bodies
  stored as parse trees and `pg_depend` are already right. Checkout of a
  commit with no source branch to copy from, and clone, make objects
  from the commit's canonical schema instead (the canonicalizer's reverse
  direction) and load pages from trees; rows read from trees carry no
  OIDs, so the new objects' OIDs are free to differ from another
  cluster's.
- **Connections choose a branch** by database name (`db/branch`, as
  built).
- **Shared catalogs** (`pg_database`, `pg_authid`, tablespaces) stay
  cluster-wide and unversioned, as decided for question 27.
- A branch that no connection uses and that has no uncommitted changes can
  drop its copies; it is then only its head commit. Its copies are made
  again when it is next used: from a branch whose head is close to it,
  then the difference applied (part 2), or, when no branch is close, from
  the commit itself.

### Clone, fetch and push

Commits carry everything needed to rebuild a branch's versioned schemas:
each prolly table's rows as trees, and every versioned object's canonical
definition by name. Clone, fetch and push transfer chunks and refs, as in
Dolt; a branch received this way is only a ref until it is first used,
when its copies are made from its head commit. Uncommitted working-set
changes are local, as in Dolt. The only things a commit doesn't carry are
the shared objects versioned ones depend on (extensions, roles, ordinary
schemas' objects); a clone checks that the receiving database has them
and refuses, naming what is missing, if it doesn't.

### What is in pages and what is in the chunk store

| | Pages (working set) | Chunk store (history) |
|---|---|---|
| Format | Postgres's: heap tuples, btree | Ours: prolly trees of canonical rows, canonical schema, root values, commits |
| Mutability | Mutable, MVCC | Immutable, content-addressed |
| Ordering | Postgres collations, opclasses | The canonical key encoding (memcmp order from btree opclasses) |
| Durability | WAL, checkpoints, full-page writes | WAL-logged appends to the store's files (below) |
| Per branch | Each versioned schema's copy: its objects' catalog rows, and its prolly relations' block maps (copy-on-write) | Refs (branch heads, working-set roots) |
| Indexes | Every Postgres index AM | Only what history needs: each table's row tree by key |

### The chunk store

Shaped like Dolt's (`noms` store), in our own formats:

- **Journal:** an append-only file of chunk records and root records, each
  with a length and a CRC; its index is an accelerator rebuilt from the
  journal when missing.
- **Table files:** immutable files of compressed chunks with a sorted
  address index and a footer, written by GC, conjoining and remote
  transfers; a second generation (`oldgen`) for chunks reachable from
  commits, as Dolt's GC does.
- **Manifest:** names the current root, the table files and the GC
  generation; replaced atomically.
- **Refs:** branches, tags and working-set roots in a map stored in the
  store itself, the manifest naming its root, as in Dolt.

**Durability (user, the recommendation for 2):** the store's files are
written in their own formats, and every change to them is WAL-logged
through a resource manager of our own, so streaming replication, base
backups and point-in-time recovery carry the history too, and a Dolt
commit is atomic with its SQL transaction:

- **Chunk appends** are logged as the journal record bytes they append.
  An aborted transaction's chunks stay in the journal, unreachable, and are
  collected by GC.
- **The root moves with the commit record.** The new root address (and
  anything time-stamped with it) is carried by the transaction's commit
  record, so redo applies it exactly when the transaction committed, and a
  standby's files are byte-identical to the primary's.
- **New table files** (GC, fetch, clone) are logged as their chunks, or as
  file contents for large transfers; **manifest replacements** and
  **unlinks** as their own records.

### Chunk contents: what carries over

Most of the canonical format built in step 4 was ours already and
carries over; only what assumed chunks live in 8 KB pages changes:

- **Hash:** SHA-256 (`hash_sha256.c`), as built.
- **Keys:** the memcmp-comparable key encoding from btree opclasses and
  collations (`keyenc_ordered.c`), as built; collation versions in the
  stored format stay a question (46).
- **Rows:** the canonical row encoding (`valenc_canonical.c`,
  `prolly_datum.h`), as built: types' binary send formats, portable
  arrays and composites, no local OIDs. Types without a binary send
  format are refused or stored as text (question 35; pending since step
  4h).
- **Chunker:** the byte-based rule already planned for step 5 (a key-only
  boundary hash salted by level, a Weibull rule around 4 KB, a minimum of
  512 bytes), with the maximum no longer tied to `BLCKSZ`: chunks live in
  the store's files, not in pages (question 45 goes away).
- **Node format:** `nodeformat_basic.c` loses its page header and becomes a
  variable-length chunk.
- **Large values:** value trees (#18), as built.
- **Schema and identity:** the canonicalizer (#23): identity by name,
  canonical schema derived from the catalogs, as built.

### What this answers

- **Step 4h's pending decisions:** types without binary send/receive
  are stored as text (question 35); unlogged tables stay unversioned; btree and other
  index AMs are each branch's own, through the block map, so a TRUNCATE on
  one branch no longer affects another; merging into a branch with
  uncommitted changes is allowed when nothing conflicts, as in Dolt
  (part 2).
- **Versioned schema** comes from per-branch copies of versioned schemas
  instead of catalogs on prolly trees:
  `09-versioned-catalogs.md` part 4's stages 2–7 are superseded. Its part 1 (canonical commits, identity by name) carries
  over.
- **The 8 KB question:** pages are Postgres's; chunks are the store's and
  no longer depend on `BLCKSZ`.
- **Remotes** use our own protocol (V4); DoltHub and Dolt's remote API are
  out of scope.

## 2. Operations

| Operation | How | Cost |
|---|---|---|
| SQL read or write | Heap and indexes on the branch's copies | Heap's |
| Create a branch at another branch's head | Copy the versioned objects' catalog rows (same OIDs, new branch, new relation file numbers), then attach each prolly relation's pages by reference | O(versioned catalog rows); no page is read |
| Create a branch at a commit | As above from a branch whose head is near it, then apply the tree diff between the two heads as row changes | O(Δ log N) |
| Dolt commit | Fold: the working-set diff (below), written into the head's trees, then the schema, root value, commit, ancestry and refs | O(changed pages + Δ log N + log H) |
| Working-set checkpoint | The same fold, published as the branch's working-set root instead of a commit | As a commit |
| Working set vs head: diff, status | The change finder (below) over the changed-page lists | O(changed pages + Δ); "changed at all?" O(1) a table |
| Diff of commits, log | Over trees: subtree-skipping diff | Dolt's bounds |
| Working set vs any commit | The working-set diff merged by key with the head-to-commit tree diff | Both bounds |
| Fast-forward, reset, checkout of a commit | Apply the tree diff from the current head to the target as row changes | O(Δ log N) |
| Three-way merge | Merge trees as Dolt does (base, the target branch's folded working set, the source), then apply the diff from the target's working set to the result as row changes; conflicts as `dolt_conflicts` rows, as built | Dolt's merge bound plus O(Δ log N) |
| Fetch, push, clone, pull | Our protocol, chunks by address; a cloned branch gets pages when first used | Dolt's bounds; first use O(N log N) materialization |

**Diffing the working set against the head (user, 2026-10-16).** Dolt
diffs a working set against its head in O(Δ log N) because both are
prolly trees; Dolt's status, diff and commit all rest on it. Pages lose
that structure, so the change finder recovers it from copy-on-write, and
it is a first-class operation: `dolt_status`, `dolt_diff` and the fold of a
commit or checkpoint all use it.

- **A baseline, distinct from the head.** Each branch has a baseline: the
  tree of its last fold, whether a Dolt commit (then it is the head) or a
  working-set checkpoint (then it isn't), and the epoch of that fold.
  Pages describe changes since the baseline; trees describe the rest.
- **Old pages are kept.** The first logical change to a page after the
  fold epoch copies the page even when no other branch shares it, so the
  page's version as of the baseline survives until the next fold. Map
  entries already carry a birth epoch for this.
- **Changed pages are listed, not searched for.** The first copy of a page
  after the fold epoch appends the page to a per-relation changed-page
  list (in the map fork, WAL-logged with the copy); the next fold clears
  it. Finding a relation's changed pages is O(changed pages), not a scan
  of its map.
- **Old rows come from the tree; old pages only name candidates.** Every
  tuple on either version of a listed page, visible or not, gives a
  candidate key (the primary key, or the content hash for keyless
  tables). A candidate's old row is read from the baseline's tree by key,
  its new row from the branch's current pages, and equal pairs drop out.
  A row an update moved to another page is on two listed pages, and
  matching by key pairs it up. Since visibility on old pages is never
  consulted, no snapshot is kept, and VACUUM, freezing and the truncation
  of transaction status need not respect old pages. A candidate whose key
  can't be read from an old version (a key value stored out of line
  that VACUUM has since removed, or any out-of-line value of a keyless
  table, whose key is a hash of the whole row) makes the diff compare
  that table in full instead: correct, but O(N) for that table.
- **A fold keeps what it couldn't see.** A fold's snapshot leaves out
  transactions still running. Pages holding tuples written or deleted by
  them stay on the list after the fold (their old versions become the new
  baseline's), so when those transactions commit, which changes no page,
  the next diff still visits them.
- **Status is O(1) a table.** A table has changed since the head if its
  baseline tree differs from the head's (one hash comparison) or its
  changed-page list isn't empty.
- **Changes that change no rows don't count.** Hint bits never copy a
  page. Pruning and freezing write a page in place and don't list it,
  unless another branch shares it (then it is copied for that reason
  alone, and not listed). Candidates come from every tuple of an old
  version, so a page changed in place this way loses none.
- **Old pages are bounded.** A branch that goes long without a commit
  keeps up to two versions of each changed page; a working-set checkpoint
  folds the changes into a tree, which becomes the new baseline, and
  frees the old versions, bounding both the space and the next diff.
- **The diffs.** The working set against the head merges, by key, the
  tree diff from the head to the baseline with the page changes since the
  baseline (when the baseline is the head, only the latter). Head against
  any commit is a diff of two trees, as in Dolt. The working set against
  any commit merges the working-set diff with the head-to-commit tree diff
  by key.

The cost of a working-set diff is O(changed pages + Δ): a changed row
touches one or two pages, so changed pages are a small multiple of Δ, and
each costs a page read rather than a few tree nodes.

**Applying a tree diff as row changes** reuses the machinery of logical
replication's apply worker (`src/backend/replication/logical/worker.c`):
inserts, updates and deletes by key with index maintenance and constraint
checks, triggers off by default (as Dolt's merge fires none). Running
transactions keep their snapshots: the applied changes are ordinary
committed row versions.

## 3. The interfaces under the pages

User, 2026-10-15: the Postgres facilities that page storage relies on may
need to become interfaces, each with a standard implementation that
behaves as Postgres does today and our own implementations beside it.
What this design needs from each:

| Facility | Interface today | Needed for this design |
|---|---|---|
| Storage manager (smgr) | Already a table of functions (`smgrsw[]`, `src/backend/storage/smgr/smgr.c`), with md.c the only entry | A second implementation, block-mapped copy-on-write storage, chosen per relation (prolly relations); md.c stays the standard one |
| Buffer manager | None | One hook: an exclusive lock on a page whose physical block is shared remaps it first, outside any critical section, so the remap is WAL-logged before the change (part 5). Hint-bit writes under a share lock stay in place (hint bits don't change any branch's rows). Buffer tags stay logical (per-branch relation file numbers) |
| Checksums | Computed with the logical block number | Unchanged: verification uses the same logical number |
| AIO and read streams | md.c's callbacks | Implemented by the new smgr, mapping each logical block range to physical ones |
| full_page_writes, WAL | Generic | Unchanged for pages; new records for block-map changes and the chunk store |
| bgwriter, checkpointer | Generic over smgr | The checkpoint also flushes the block maps and syncs the journal |
| pg_stat_io | Generic | Unchanged |

Each new piece sits behind an interface with a stub and an implementation,
as `00-architecture-principles.md` requires: the page store (md.c, block-
mapped), the branch page-set manager, the change finder, the row applier,
and the chunk store (journal and table files). The format layers already
are interfaces (hash, key and value encoders, chunker, node format).

## 4. From today to there

### Where things are

As this plan was written (2026-10-15):

- **Merged on `master` (PRs #1–#11):** the interface stubs and registry,
  D1 tables and the prolly index AM, the copy-on-write tree, the
  cluster-wide node store, node counters and the subtree-skipping diff.
- **Open, stacked (PRs #12–#46):** the rest of step 4: snapshots, K1 row
  locks, spill, SERIALIZABLE, large values, GC, VACUUM, branches, diff and
  merge, canonical commits, the `dolt_*` SQL interface, conflicts,
  versioned databases, and the 16 fixes from Postgres's own suites (4h).

As of 2026-10-17, steps 0–5 below are built and merged; each step says
with which PRs.

### What carries over

| Today | In the new design |
|---|---|
| `dolt_*` functions, views, `dolt.author`, `db/branch` connections, conflicts as in Dolt (#25, #26) | Kept; their backend changes to per-branch copies and the chunk store |
| `CREATE DATABASE … VERSIONED` and its refusal of heap tables (#27) | Superseded: version control is per schema (versioned schemas), so any database can hold both. Removed with D1 |
| Hash, key encoding, canonical rows, value trees, canonicalizer (4a, #18, #23) | Kept: the chunk store's contents |
| Copy-on-write trees, subtree-skipping diff, three-way merge, merge conflicts, refs, commits (4c, #9, #21, #22, #26) | Kept; they operate on chunks from the new store instead of node-store pages |
| Chunker, node format | Reworked: the byte-based rule (step 5's plan) and pageless nodes |
| Interface registry and stub pattern (`prolly_registry.c`) | Kept; the new interfaces register there |
| Requirements suite (`src/test/prolly`), `test_prolly`'s version-control tests, the benchmark harness, gates 1–7 | Kept; storage-engine tests go with D1 |
| The node store (`nodestore_global.c`), reclamation (#19) | Replaced by the journal and table-file store and its GC |
| D1 and K1 (table AM on trees, row locks, TID maps, keyings, spill, SERIALIZABLE at table granularity, the 4h fixes to them), the prolly index AM | Superseded by pages. Kept building until the new design is measured against them (step 8), then removed |
| `09-versioned-catalogs.md` part 4 stages 2–7, and #34 (catalog writes through the table AM) | Superseded by per-branch copies of versioned schemas; #34 stays harmless |

### Steps

Each step is a stack of PRs with its own tests (every bug with a test that
fails without its fix), and Postgres's suites keep passing after each one.

**Step 0. Settle the open stack and these specs.** Done 2026-10-15.
- #12–#47 merged; the D1 engine's final state is tagged `prolly-d1`.
- Types without a binary send format are stored as their text output
  (user, question 35).
- Shared catalogs stay cluster-wide and unversioned (user).

**Step 1. Block-mapped storage and the `prolly` table AM.**
- Make smgr's implementation selectable per relation, md.c the default.
- A block-mapped implementation (part 5): per-fork map trees, a page
  store per database, allocation and free-space reuse, WAL, redo,
  checkpoints, and AIO reads.
- The `prolly` table access method: heap's implementation on block-mapped
  storage, with its indexes and TOAST tables block-mapped too. D1's index
  AM, which holds the name `prolly`, becomes `prolly_d1_index`.
- No branches yet, so no sharing and no remapping.
- **Exit:** with `default_table_access_method = prolly`, gates 1–3 match
  stock Postgres; crash, standby and base-backup tests of the map pass.
- **Built 2026-10-15:** #50 (smgr per relation), #52 (block-mapped
  storage), #53 (the `prolly` table AM); design in #49, part 5.

**Step 2. Branches: per-branch copies of versioned schemas.**
- `CREATE SCHEMA … VERSIONED`; prolly tables only there, and only prolly
  tables there; the one-way dependency rule, checked in `pg_depend`.
- The branch column in the catalogs that hold versioned objects, their
  unique indexes, catalog scans filtered by the session's branch, and
  cache resets on a change of branch.
- Making a branch's copies: catalog rows copied with the same OIDs, new
  relation file numbers, pages attached copy-on-write.
- Branch creation copies block maps by reference and flushes the source's
  dirty buffers first; remapping shared pages (part 5); freeing a copy's
  pages past the xmin horizon; VACUUM and freezing on shared pages
  (visibility-neutral changes in place; line pointers made unused only
  when no other branch shares the page).
- `dolt_branch` and `dolt_checkout` on copies, no commits yet.
- Locks on a versioned object's copy are the branch's: relation, tuple,
  page and predicate locks carry the branch, so sessions on different
  branches don't wait for each other (user, 2026-10-16).
- Making a branch takes `CREATE` on the database; deleting one takes
  owning it (user, 2026-10-16).
- Known limit, accepted (user, 2026-10-16): a standby's sessions don't
  hold up a branch deletion replayed there.
- **Exit:** DDL and TRUNCATE on one branch leave the others alone, for
  tables and every other object kind; ordinary schemas are unaffected by
  branches; a shared object depending on a versioned one is refused;
  branch creation reads no table's pages; sessions on different branches
  don't block each other; the S suite on a branch matches heap.
  (`vc_schema` moved to step 4's exit, user 2026-10-16: it needs
  commits.)
- **Built 2026-10-15–16:** #59 (versioned schemas), #60–#61 (branch
  columns, branch-aware catalogs), #69–#70 (shared pages, copy on first
  write), #71 (branches as copies), #72 (maintenance across branches), #91
  (locks per branch); design in #54, #56 and #58, part 6.

**Step 3. The chunk store, WAL-logged.**
- The journal and its index, table files, the manifest and the refs map,
  in the directory of each database with prolly tables;
  concurrent appends from backends serialized by a lock.
- The resource manager: chunk appends, the root in the commit record,
  table files, manifests, unlinks; redo and base-backup consistency.
- Pageless nodes and the byte-based chunker; the tree, diff and merge
  code reading and writing through the new store.
- Table files compress chunks with LZ4, or not at all where Postgres is
  built without it (user, 2026-10-16).
- Keys ordered by a collation record its version in the commit; reading
  a commit whose collation version differs from the database's is
  refused (user, 2026-10-16).
- **Exit:** the tree tests of `test_prolly` pass on the new store; crash
  tests at every record boundary recover to the last committed root; a
  standby's store matches the primary's byte for byte.
- **Built 2026-10-16–17:** #92 (the chunk store and its resource
  manager), #93 (trees as chunks), #110 (table files and the manifest).
  Collation versions are recorded with #100 in step 4.

**Step 4. Dolt commits from pages.**
- The fold over the branch's versioned schemas: the change finder and
  tree edits for prolly tables, the canonical schema for every object
  (the canonicalizer in both directions, the reverse for checkout and
  clone), commit and ancestry, refs and working-set roots; working-set checkpoints.
- `dolt_commit`, `dolt_log`, `dolt_status`, `dolt_diff` and the views on
  the new store.
- Fold epochs, copy on first change after the fold, the changed-page
  list and its counter (part 2), with pruning and freezing in place on
  unshared pages (moved from step 2, 2026-10-15: they serve the fold, and
  the list's storage and what a fold keeps on it are decided with it).
- The working-set diff (part 2) behind `dolt_diff`, `dolt_status` and
  the fold: baselines distinct from heads, candidates from old pages,
  old rows from trees, pages of transactions running at a fold kept.
- Decided (user, 2026-10-16):
  - `dolt_commit` in an open transaction folds that transaction's own
    writes too, and the Dolt commit is part of the transaction: it is
    gone if the transaction rolls back.
  - Working-set checkpoints are taken when asked for and when a session
    leaves a branch with changes, nothing automatic until measured.
  - The canonical format (hash, keys, rows, nodes) freezes at the end of
    this step; every chunk carries a format version byte.
  - A keyless table's diff may fall back to comparing the table in full
    (O(N)) where a candidate's key can't be read.
- **Exit:** gate 6 (history independence) and the requirements suite's
  commit and history tests (`vc_schema` among them) pass; commit, diff
  and status cost track changed pages (counted, as the tree diff's node
  reads are).
- **Built 2026-10-16–17:** #95 (the fold), #96 (commits of branch
  copies), #98 (status and diff), #99 (requirements), #100 (collation
  versions), #111 (fold epochs and the changed-page list), #112 (folds of
  only the rows changed), #113 (working-set checkpoints), #114 (the
  format frozen).
- **As built:** a table without a primary key was read in full on any
  change (issue #126); since #136 its rows' copies are counted from the
  changed pages too, and it is read in full only where that can't be told
  (a fold in the transaction that wrote its rows leaves the next fold to
  read it in full, among others).
- **As built:** `dolt_status` and `dolt_diff` failed on objects a commit
  can't hold yet (partitioned and foreign tables, aggregates, policies),
  and a diff of one table folded every changed table (issue #130); since
  #137 reads leave such objects out (partitions with their partitioned
  tables), `dolt_status` lists them as "not covered" and unstaged, a branch
  with them is dirty, only a commit refuses them, and a diff of one table
  folds that table alone.

**Step 5. Moving heads.**
- The row applier (from logical replication's apply worker); checkout and
  reset to a commit, fast-forward, branch at a commit, materializing a
  branch with no pages.
- Three-way merge over trees into a working set with changes; conflicts
  as built; cherry-pick, revert.
- Decided (user, 2026-10-16):
  - Reset, merge and checkout of a commit on a branch other sessions
    use behave as in Dolt: the branch's working set changes under them,
    as a committed transaction's changes do.
  - Rows written by merge, reset and fast-forward fire no user triggers
    and recheck no foreign keys, as logical replication's apply does;
    merge records constraint violations as `dolt_constraint_violations`
    rows, as Dolt does.
  - Schema merge follows Dolt: additive changes merge, the same object
    changed differently on both sides is a schema conflict, and a
    conflicting column type change fails the merge.
  - Checking out a commit (not a branch) gives a detached, read-only
    session; changing anything takes a branch made from it.
  - A merge gives each sequence the larger of its two values.
- **Exit:** `vc_branches`, `vc_diff_merge`, `vc_history` and the
  conflicts tests pass; V1–V3 run, V3 with background writes.
- **Built 2026-10-17:** #101 (the row applier), #102 (reset,
  fast-forward, branches at commits), #104–#105 (three-way merge,
  conflicts, requirements), #106 (cherry-pick, revert), #107 and #120
  (moves across definitions, types and materialized views included),
  #108 (sequence values), #109 and #119 (merges across definitions),
  #116 (merges into uncommitted changes), #117 (cherry-pick conflicts),
  #118 (`dolt_constraint_violations`), #121 (detached checkout), #122
  (the cross-cluster history test), #123 (the V suite), #124 (head-only
  branches).
- **As built:**
  - A detached checkout is a hidden branch at the commit, read-only for
    the session on it. A checkout that leaves one deletes every hidden
    branch no session is on, but only those the session may delete
    (deleting a branch takes owning the database): one left by a session
    that may not stays until one that may checks out.
  - Every new branch has only its head until first used, whether made
    from a branch (`dolt_branch`, `dolt_checkout -b`) or at a commit; only
    one made from a branch with no commits is copied at once. Its first
    use makes its copies.
  - Making a branch takes what it always did: a valid name, CREATE on the
    database and a transaction that can write; deleting one, owning the
    database and a transaction that can write, head-only or not. The
    first use takes none of these: a role without CREATE, or a read-only
    transaction, can check out or connect to a branch someone else made.
    A standby refuses a branch's first use until the primary has made it.
  - With uncommitted changes, a merge (a fast-forward too), cherry-pick or
    revert is refused, as Dolt refuses it, when it would change a table,
    sequence or materialized view that has uncommitted changes. Unlike
    Dolt, it is also refused whenever definitions change, whether
    uncommitted or on either side of the merge, even without a conflict
    ("cannot merge with uncommitted changes where definitions change").

**Step 6. Remotes.** Designed in part 7 (decided 2026-10-17). File remotes
built (#171, #172, #174, #175; 7.11); network remotes and V4 not yet.
- File remotes first, then network remotes over libpq: fetch, push,
  clone, pull; a cloned branch's copies made on first use from its head
  commit; the check for shared dependencies (part 1).
- **Exit:** V4.

**Step 7. Reclamation.** Designed in part 8 (decided 2026-10-17).
- Chunk-store GC with WAL (table files, `oldgen`, journal retirement);
  page-set and physical-page reclamation; the fold epoch's retained page
  versions.
- **Exit:** space returns after branch deletion and GC; crash tests of GC.

**Step 8. Measure, then retire D1.**
- Gates 1–7 and the S and V suites for this design against stock heap and
  against D1; `10-complexity-requirements.md` rechecked operation by
  operation.
- Then remove D1, K1, the prolly index AM, the node store and anything
  else superseded, in their own PRs, and update the specs.

### Risks to retire early

- **Shared pages and VACUUM.** Pruning and freezing on a page another
  branch shares must not change either branch's rows or break the other
  branch's index entries. Step 2 settles the rules before anything
  depends on them.
- **The fold's change finder** must see every changed row, including rows
  moved by updates across pages and by VACUUM; step 4's tests include
  HOT, non-HOT and pruned pages.
- **Two buffers for one physical page** (one per branch sharing it) cost
  cache, not correctness; measured in step 8.

## 5. Step 1 design: the block-mapped storage manager

Built in `src/backend/storage/smgr/bmap.c` (2026-10-15); this part
describes it as built.

### Selection

- smgr already dispatched through `smgrsw[]`; `smgropen()` now asks each
  implementation after md.c whether it holds the relation
  (`smgr_claims`), falling back to md.c. A relation is block-mapped when
  it has a map fork (below), so the choice needs no catalog access and
  works the same in backends, I/O workers, the startup process and tools.
- Creating storage says which kind: `RelationCreateStorageWith()` and
  `smgrcreatewith()`. Indexes are created in their table's storage
  manager; storage made anew (TRUNCATE, REINDEX, SET TABLESPACE, CREATE
  DATABASE's copies, a rewrite's new heap) stays in the storage manager
  that held the old. Only permanent relations outside the global
  tablespace are block-mapped; anything else is md.c whatever the caller
  asks, so `SET UNLOGGED` moves a table to md.c.
- Until the `prolly` table AM exists, the developer setting
  `debug_block_mapped_heap` puts new permanent user heap tables (and with
  them their indexes and TOAST tables) in block-mapped storage. The gates
  run with it (`make -C src/test/prolly check-upstream-bmap`).

### Files

| File | What | Accessed through |
|---|---|---|
| `<relfilenumber>_bmap`, the map fork (a new fork, `BMAP_FORKNUM`) | Block 0: the relation's logical length. Block 1 + L / 1,019: the entry of logical block L (physical block, birth epoch) | The buffer manager, as an ordinary md.c fork: WAL, full-page writes and checkpoints apply unchanged |
| The page store (relation file number 9421), per tablespace and database | Every block-mapped main fork's pages, at physical block numbers | md.c directly, never the buffer manager |
| The allocator (9420), per tablespace and database | A metapage (the page store's high-water mark, free count, bitmap pages) and bitmap pages of used blocks | The buffer manager, as an ordinary md.c relation |

- **Direct addressing, not a tree.** A block's entry is found without
  following any pointer. In recovery, map pages on disk can come from
  different moments (one written after the point redo has reached,
  another not yet written); a first design with a radix tree failed there
  (`026_overwrite_contrecord`: a root already grown pointed at a node not
  yet written). An entry never changes once set, so any version of its
  page that has it gives the same answer, and a page redo needs has it.
- **Only the main fork is block-mapped.** The free space map and the
  visibility map are md.c files of the relation: they are extended during
  recovery without WAL records of their own, so a standby can't follow a
  primary's allocations for them, and they are hints or rebuildable, so
  they need no sharing. A branch's copy (step 2) starts with an empty
  visibility map, which is always correct.
- The map fork is hidden from callers that walk every fork (copying a
  relation, measuring it): `smgrexists` says no for it.
- The page store's pages carry checksums of their *logical* block
  numbers (the buffer manager computes and verifies them), so tools that
  verify checksums from files skip it; map forks and the allocator verify
  normally. `pg_checksums --enable` refuses a cluster with block-mapped
  storage.
- A dropped relation's map fork stays until after the next checkpoint, as
  md.c keeps a main fork's first segment, so its relation file number
  isn't reused while crash recovery may still replay records for it.
  Dropping a tablespace removes a database's allocator and page store
  there once no map fork remains.
- `pg_relation_size` reads a block-mapped relation's length from its map.

### Allocation

- Physical blocks are allocated from the bitmap, preferring freed blocks,
  otherwise extending the page store at its high-water mark. Reused
  blocks are zeroed before they are mapped, so a crash can't expose a
  page's old contents at a new logical block. Truncation only shortens
  the logical length; blocks past it are reused by a later extension or
  freed with the relation.
- Epochs in map entries are unused in step 1 (no sharing); they are in the
  format now so step 2 can decide sharing without rewriting maps.

### WAL and crash safety

The map is replicated state, identical on a primary and its standbys
(as md.c's files are), so base backups and standbys work unchanged. Every
map change is logged by the `BlockMap` resource manager, registering the
map pages it changes, so torn pages are restored by full-page images; a
record's per-page deltas are applied by the same code in normal running
and in redo:

- **Create**: the map fork's header.
- **Extend**: allocation, leaf entries, the length, the bitmap and the
  high-water mark. Logged before the new blocks are used, and redo never
  allocates: a data record for a new block always follows its
  allocation. Callers that log a page's image before writing it (the bulk
  writer behind index builds and copies, and hash indexes' bucket
  allocation) call the new `smgrpreextend()` first. Redo zeroes reused
  blocks and extends the page store again, except for a relation whose
  data skips WAL (`wal_level = minimal`, created in the same
  transaction): its pages were synced at commit, and zeroing them would
  lose them (`018_wal_optimize`).
- **Truncate**: the length, inside `RelationTruncate`'s critical section,
  with the header pinned beforehand by the new `smgrpretruncate()`.
- **Free**: after commit or abort, a dropped relation's length is cleared
  and its blocks freed, in records of their own. A crash between the
  commit and these records leaks the blocks until reclamation (step 7).

### I/O

- Reads and writes translate logical runs to physical runs and call md.c
  on the page store; `smgrmaxcombine` reports the physically consecutive
  run, so combined reads never cross a gap.
- Asynchronous reads are started on the page store's md.c relation, so an
  I/O worker reopening the target needs no map. The map is read before the
  read's I/O handle is acquired, through the new `smgrprepareread()`: a
  backend can hold only one handle.
- Writing a page looks it up without loading map pages into shared
  buffers: a cached map page is used, otherwise the page is read from
  disk into private memory, where an uncached page is current. Otherwise
  a write could need an eviction whose write needs a lookup, and so on
  until no buffer is left (seen with a 1 MB buffer pool). The background
  writer and checkpointer read map pages, so `pg_stat_io` counts their
  reads, hits and evictions of relation pages.
- A relation's extensions (and its map fork's growth) are serialized by
  its map header's exclusive lock, the allocator's growth by its
  metapage's: not by relation extension locks, since a backend extending a
  relation holds that relation's extension lock and may take no other
  heavyweight lock.
- Sync requests are md.c's, for the page store's and the map's segments;
  `smgrimmedsync` syncs the page store.

### Results (2026-10-15)

With `debug_block_mapped_heap` (`make -C src/test/prolly
check-upstream-bmap`):

- Regression suite: 239 of 239.
- Isolation suite: 134 of 134.
- Recovery suite: everything passes, `027_stream_regress` (the regression
  suite on a primary and a replaying standby) included, but for checks
  that read relation files directly: `016_min_consistency` reads a
  table's main-fork file to compare page LSNs, and two checks of
  `018_wal_optimize` expect one numeric file per relation. A block-mapped
  relation has no main-fork file, and the allocator and page store are
  numeric files no relation names.

`test_prolly`'s `028_bmap_storage.pl` covers crashes (including a lost
first extension), a standby, promotion, block reuse, base backups,
`pg_checksums`, rewrites, persistence changes and tablespaces.

### The `prolly` table access method (2026-10-15)

`prolly` returns heap's own `TableAmRoutine`, so code that recognizes heap
by its routines treats a prolly table as heap; heap's storage creation
puts a relation whose access method is `prolly` (and its TOAST table and
indexes, which follow their table) in block-mapped storage. Prolly tables
are permanent: with `prolly` as the default, temporary and unlogged tables
get heap; `USING prolly` on one, `SET UNLOGGED` on a prolly table and
`SET ACCESS METHOD prolly` on an unlogged one are refused. D1's index
access method, which held the name, is now `prolly_d1_index`.

With `default_table_access_method = prolly`: isolation 134 of 134;
regression 236 of 239, the three others (`identity`, `alter_table`,
`publication`) turning default-AM tables unlogged, which is refused; the
recovery suite as with `debug_block_mapped_heap`, plus
`027_stream_regress`, which runs the same regression suite.

### Step 2's remapping rule (designed now, built then)

A shared page must be copied before its first change is WAL-logged, and
changes are WAL-logged inside critical sections, where allocating and
logging a remap can't happen. So the remap happens when a backend takes an
exclusive content lock (or a cleanup lock) on a buffer whose block is
shared, before any critical section: allocate, log the remap, point the
buffer at the new block. Redo never decides remaps; it replays them, and
the primary's order (remap, then change) holds. Creating a branch flushes
the source's dirty buffers first, so a shared block on disk holds the
state both branches start from. An exclusive lock taken without a change
copies a page needlessly, which costs space, not correctness; heap's
opportunistic pruning skips shared pages.

### Known limits after step 1

- `pg_rewind` and `pg_upgrade` don't understand block-mapped storage yet.
- Freed blocks are reused, but the page store never shrinks.
- Online checksum enabling skips map forks, which `smgrexists` hides.
- Each I/O looks up the map (a copied map page or two); a buffer header
  that remembers its physical block would save the lookup on writes.
  Measured in step 8.

## 6. Step 2 design: branches as copies of versioned schemas

Proposed 2026-10-16; built as a stack, one PR per part, in this order.

### 6.1 Versioned schemas

- `CREATE SCHEMA name VERSIONED` sets `pg_namespace.nspversioned`.
  Turning an existing schema versioned (or not) is refused for now.
- A relation created in a versioned schema gets the `prolly` access
  method whatever the default; another explicit access method, `UNLOGGED`,
  and `ALTER TABLE … SET SCHEMA` between a versioned and an ordinary schema
  are refused. `USING prolly` outside a versioned schema is refused.
- An object is versioned if its schema is, or, for an object without a
  schema (a column, constraint, trigger, rule, policy, default, index
  entry, enum label, TOAST table), if the object it belongs to is.
  `object_is_versioned(classid, objid)` answers this from the catalogs.
- **The dependency rule** is checked in `recordMultipleDependencies()`:
  a dependency from a shared object to a versioned one is an error, so it
  covers every kind of dependency Postgres records. Extensions are shared;
  `CREATE EXTENSION … SCHEMA` into a versioned schema is refused.
- **Foreign keys stay on one side** (built 2026-10-16). A foreign key puts
  triggers on both tables, so even one from a versioned table to a shared
  table would leave the shared table checking every branch's rows; the
  dependency rule catches it (the referenced table's triggers depend on
  the versioned constraint), and the foreign key code refuses it first
  with a clearer message. This narrows part 1, which allowed references
  from prolly tables to shared tables.
- `default_table_access_method = prolly` means heap outside versioned
  schemas; only an explicit `USING prolly` there is an error.
- A function whose body is a string (not `BEGIN ATOMIC`) records no
  dependency on what it queries; it resolves names when it runs, so a
  shared function sees the session's branch.

### 6.2 Branches and sessions

- **`pg_branch`**, a database-local catalog, lists the database's
  branches: an OID (the branch id used in catalog rows), a name, and
  later its head and baseline. `main` is a bootstrap row with a fixed OID.
  Branch id 0 means "shared": no branch.
- A session's branch, `MyBranch`, is `main` unless the connection named
  another (`dbname/branch`, as D1 sessions do; a database D1 versions
  keeps D1's meaning). `dolt_checkout()` changes it and resets the
  backend's caches as an invalidation overflow does.

### 6.3 Branch-aware catalogs

- **Which catalogs:** every database-local catalog that can hold a
  versioned object's rows gains a branch column (`<prefix>branch`, type
  `oid`, default 0), listed in one table (`catalog/branch.c`) with each
  catalog's rule for deciding a row's branch:
  - a schema column (`pg_class.relnamespace`, `pg_type.typnamespace`,
    `pg_proc.pronamespace` and the like): the row is versioned if its
    schema is;
  - an owning-object column (`pg_attribute.attrelid`, `pg_index.indexrelid`,
    `pg_trigger.tgrelid`, `pg_enum.enumtypid`, `pg_depend`'s depender,
    `pg_description`'s object, …): the row is versioned if that object is;
  - TOAST tables and their indexes, which live in `pg_toast`, are
    versioned if their table is: `create_toast_table()` passes its parent.
- **Unique indexes** of these catalogs gain the branch column as their
  last key, so a versioned object's rows can repeat its OID and name once
  per branch.
- **Writes:** `CatalogTupleInsert` and `CatalogTupleUpdate` set the branch
  column by the catalog's rule: `MyBranch` for a versioned object's row,
  0 otherwise. Callers don't change.
- **Reads:** heap scans and index fetches of these catalogs skip rows
  whose branch is neither 0 nor `MyBranch`, in heapam (the catalogs are
  heap tables), so `systable_*`, `heap_getnext` loops and the caches built
  on them all see one branch. A relcache flag marks the catalogs and the
  column, so other relations pay one test.

### 6.4 Copy-on-write pages

- Block-map entries gain meaning for their birth epoch: a relation fork's
  map header holds its sharing epoch, and an entry born before it may be
  shared with another branch's copy.
- **Remapping** (part 5's rule): an exclusive content lock on a buffer
  whose page is shared first copies the page to a new physical block and
  logs the new entry, outside any critical section.
- **Fold epochs and changed-page lists** (part 2) are built in step 4,
  with the fold that uses them: nothing folds before then.
- **Reference counts:** a physical block shared by several maps is freed
  when the last map that holds it drops it.  The counts are a file of
  their own per tablespace and database (relation file number 9419,
  beside the allocator and the page store), one byte per physical block:
  how many maps hold it besides the first, saturating at 255, after which
  the block is never freed until reclamation in step 7 sweeps.  (Kept
  apart from the allocator's bitmap pages, which hold one bit per block:
  a byte there would cut the blocks a bitmap page covers, and so the page
  store's size, eightfold.)

### 6.5 Making a branch

- `dolt_branch(name)` flushes the session branch's dirty buffers of its
  versioned relations, inserts a `pg_branch` row, copies every catalog row
  whose branch is `MyBranch` with the new branch's id (same OIDs), gives
  each copied relation a new relation file number whose map is a copy of
  its source's with every entry marked shared, and raises the source's
  sharing epoch. Cost: O(versioned catalog rows + map pages).
- `dolt_branch_delete(name)` deletes the branch's rows and drops its
  relations' storage; shared physical blocks survive in their other maps.

### 6.6 Maintenance on shared pages

- Hint bits aren't written to a shared page: one may be true of one
  branch only (an index entry dead there only), so it stays in the
  branch's buffer and goes with the page when a change copies it.
- Pruning and freezing remap a shared page first (they write WAL), so
  they never change a page another branch can see.
- Opportunistic pruning (on a read) skips shared pages, so reads don't
  copy pages.

## 7. Step 6 design: remotes

**Status: decided by the user, 2026-10-17, as recommended** (7.9 lists
the decisions); not built.

Remotes move history between databases: a database's commits, the trees
they name, and its branches' heads. A working set, the pages, never moves
(as in Dolt); a branch that arrives is head-only until first used, when
its copies are made from its head commit (#124's path). Remotes come
first as directories (file remotes), then over the network. The bounds
are part 1's and `10-complexity-requirements.md`'s: fetch and push
O(E + B) (chunk references examined, bytes moved), clone O(B) plus the
first use's pages and index builds.

Dolt's remotes (2.4.2, `go/store/datas/pull`, `go/store/nbs`,
`go/libraries/doltcore/remotestorage` and `remotesrv`) were read for this
design; what is taken from them, and what is done otherwise, is said
where it applies and listed in 7.10.

### 7.1 What moves

Chunks, by hash: commits, the canonical roots they name, and the trees
under them.

**The closure invariant.** A store that has a chunk has every chunk it
references. Every transfer below relies on it: a chunk the receiver has
needn't be sent, nor anything under it. It is checked wherever chunks
enter a store from outside, not assumed:

- **Admission check.** Before a received table file is added to the
  manifest, every reference in its chunks is resolved, in the store or
  in the file itself; one that resolves in neither refuses the whole
  transfer, naming it. One `has()` a reference, a small part of moving
  the bytes. Always, an empty receiver included: Dolt skips its check
  (`refCheckAllSources`) when the receiver's root is empty, so a clone
  trusts its source; here it doesn't.
- **Each chunk re-hashed.** Its hash is recomputed from its bytes and
  compared with the address it came under, and its format version byte
  checked. (Dolt checks a CRC and the file's structure, but labels the
  bytes with the hash the sender gave, `CompressedChunk.ToChunk`.)
- **The format.** A root's `"!:format"` item must be this build's
  (#149's clean break): a root of another format refuses the transfer,
  naming it.
- **Checked before visible.** A received file is checked in full before
  it is renamed into place and logged as added; a check that fails
  leaves nothing the store reads. (Dolt's file-backed server renames an
  uploaded file into place before the reader whose close checks its
  length and MD5 is closed, and drops that close's error.)

Garbage collection (step 7) must keep the invariant across a transfer:
a chunk a receiver answered "have" for may not be collected before the
transfer commits, or the admission check after it refuses the transfer
(safe, but a failure). Dolt pins each such chunk for the GC in progress
(`gcDependencyMode_TakeDependency`); step 7 needs the same, and step 7's
design follows this one so that it can say how.

**Finding what to send: the walk.** The receiver walks from the heads it
wants, breadth-first by discovery: each chunk received, its references
are queued; queued hashes are checked against the receiver's store in
batches (`has()` of many at once), each hash once (a seen set), and only
the absent are fetched. A chunk the receiver has ends that path. Checks,
fetches, decoding and writing overlap, as Dolt's puller's do
(`PullChunkTracker`: batches of 64K, three in flight). O(E) references
examined. The seen set is bounded by `maintenance_work_mem`, spilling
as a hash join does past it (Dolt's is unbounded).

**Finding what to send: from known heads.** The walk learns a commit's
parent only once it has the commit, so k new commits in a line cost k
round trips to the remote however the rest is overlapped; Dolt has that
cost. A remote here runs this code, so a fetch can do it in one: the
receiver names the heads it already has from that remote (its
remote-tracking heads), the sender walks the commit graph from the
wanted heads down to those (O(new commits)), and sends the chunks of
each new commit's root that the known roots don't have, found by the
tree diff that skips subtrees with equal hashes (step 4's). Push is the
same the other way. Correctness doesn't rest on the sender's guess: the
admission check refuses a transfer that leaves anything unresolved (a
known head the receiver has since lost, say, to a forced fetch's GC),
and the receiver falls back to the walk. This is git's negotiation, not
Dolt's.

**Sent as stored.** A remote sends chunks as its files hold them,
compressed or not (step 3's table files, LZ4 or not, #110), without
decoding: a sender reads byte ranges of its table files, and ranges
within 8 KB of each other are read as one (Dolt's `ranges.Tree`
coalesces at that gap).

**Received as table files.** What one transfer receives is written as
new table files, then added to the manifest at once, in one transaction
with the refs it moves (7.7); not appended to the journal chunk by
chunk. A transfer that fails part way leaves its files unreferenced, for
step 7's GC to remove; none are read.

### 7.2 Remote refs

- **Remotes** are named, per database, as Dolt's are:
  `dolt_remote('add', name, url)`, `('remove', name)`, listed in a
  `dolt_remotes` view; kept in the refs tree (a new tag), not in a catalog,
  so they move with the refs' transactions.
- **Remote-tracking branches** (`origin/main`): heads only, written by a
  fetch, never with copies; `dolt_checkout('origin/main')` is refused with
  a hint to make a branch from it, and `dolt_checkout('-b', 'x',
  'origin/x')` makes one at its head (head-only, as any new branch).
- **Upstreams**: a branch may name one (`dolt_push('--set-upstream', …)`,
  `dolt_branch('--set-upstream-to', …)`), for `dolt_pull()` and
  `dolt_push()` without arguments.

### 7.3 Fetch, pull, push

- **Fetch** (`dolt_fetch([remote[, refspec]])`): the walk, the chunks
  received, then the remote-tracking heads moved, in one transaction.
- **Pull** (`dolt_pull([remote[, branch]])`): fetch, then
  `dolt_merge('origin/x')` into the session's branch, with merge's rules
  (a fast-forward when it can, conflicts as merge has them, refused over
  conflicting uncommitted changes).
- **Push** (`dolt_push([remote[, branch]], ['--force'])`): the remote is
  sent what it lacks, then asked to move its branch's head, which it
  does as a compare-and-swap of the head it was told about (the chunk
  store's `move_root` is already one), and only as a fast-forward unless
  `--force`. The ref is compared and moved inside a compare-and-swap of
  the whole refs root, as Dolt's are (`database.update`,
  `doFastForward`): a concurrent push to another branch retries, one to
  the same branch fails. A branch the remote has copies of is moved there
  as a fast-forward is (`fast_forward_local`, #142), its rows written
  into its pages. Recommendation: refuse the push, in the same
  transaction, when that branch's working set has uncommitted changes
  (its changed pages differ from its head: the change finder's "anything
  changed?"), whoever made them, as Dolt's sql-server does
  (`ErrDirtyWorkspace` under `PUSH_CONCURRENCY_CONTROL_ASSERT_WORKING_SET`);
  `--force` moves it anyway, resetting them. **Decided: refuse.** The
  check is the change finder's, which reads no pages on a clean branch
  (#142); a test shows it stays so.

### 7.4 Clone

`dolt_clone(url)` in a database with no versioned schemas: every branch
fetched, each a head-only branch at its remote's head, `origin` set, and
the session checked out on the remote's default branch, which makes its
copies (the one O(N) step: rows written to pages, indexes built).

- **Whole files.** The receiver is empty, so there is nothing to walk
  against: the remote lists its manifest (root and table files) and the
  receiver copies the files whole, a few at a time, its journal as a
  table file made while it is read; then every file is checked (7.1:
  re-hashed, references resolved) before any is added. As Dolt's
  `pull.Clone`, with its fallback: when the journal is a large part of
  the store (a fifth, `canSyncRootsWithClone`), the walk instead. A
  whole-file clone brings the remote's garbage too, until the receiver's
  GC.
- **Never over a root.** A clone into a database whose store isn't empty
  is refused; it never overwrites a root (Dolt's clone and backup set
  the root over whatever is there, after ten tries).

- **Shared dependencies** (part 1): before anything is written, the
  head's canonical root is checked for what versioned objects need from
  outside them: extensions (name and version), roles named as owners or
  in grants, and objects of ordinary schemas. A clone missing any refuses,
  naming each.
- **Options for where:** (a) into an existing, empty database,
  `CREATE DATABASE x; \c x; SELECT dolt_clone(url);`; (b) a new database
  made by the clone, `SELECT dolt_clone(url, 'x')`, which needs a
  connection to it (CREATE DATABASE can't run in a transaction block, so
  not inside the function). **Recommendation: (a)**: it needs nothing new
  of Postgres, and the empty database's settings (encoding, locale,
  collation provider) are the user's choice. **Decided: (a).** A
  one-step clone, if wanted, is a client tool that runs `createdb` and
  then `dolt_clone`, as `pg_basebackup` is a client; not the server.
  Canonical form is UTF-8
  whatever the encoding, so encodings may differ (`status_diff.sql` reads
  commits in LATIN1 and WIN1251 databases); a collation whose version
  differs from the one a commit recorded makes that table's rows
  unreadable there, as reading such a commit does now (step 3).

### 7.5 Transport

- **File remotes first** (`file:///path`): a directory holding a chunk
  store's files, a manifest, table files and the refs, readable and
  writable by the server's user. Fetch reads table files from it; push
  writes a table file into it and moves its refs with a lock file. They
  need no network code, test everything above, and back up history.
- **Network: options.**
  - **(A) SQL functions over a libpq connection.** The receiving backend
    connects to the remote server, as `postgres_fdw` and logical
    replication's workers do, and calls functions there: the remote's
    heads, `has` for a batch of hashes, the chunks of a batch streamed
    with `COPY … TO STDOUT (FORMAT binary)`, and a push's head move. It
    reuses everything Postgres has: `pg_hba.conf`, roles and passwords,
    TLS, connection strings (`postgresql://host/db`), no new port or
    daemon.
  - **(B) A protocol of our own, over its own port** (as Dolt's remotes
    are: a gRPC service whose table files move over HTTP): faster for
    bulk, and servable from object storage, but a new service to run,
    secure and authenticate.
  - **(C) Object storage** (`s3://`): a file remote's layout in a bucket;
    later, as a file remote's variant.
  - **Decided: (A)** after file remotes, behind the remote interface so
    (B) or (C) can be added without changing fetch's logic. It is the
    Postgres-native choice (a remote is another Postgres database) and
    has authentication and encryption already; COPY's binary stream is
    near what (B) would move. And since the remote runs this code, (A)
    gets the one-round-trip transfer from known heads (7.1), which a
    passive store (Dolt's remotes, (C)) can't serve. (C) can follow
    without changing (A); then Dolt's download machinery applies
    (concurrency raised and halved by success and failure, a throughput
    watchdog, URLs refreshed), which over one libpq stream doesn't.
  - **On the remote,** a transfer holds a backend and a connection; its
    functions stream in bounded batches and read the chunk store, not
    tables, so they hold no snapshot that keeps back the remote's VACUUM.
  - **File remotes' layout** is a bucket's too: immutable table files
    named by content, and one manifest moved by compare-and-swap, so (C)
    is a transport, not a format.

### 7.6 Privileges

History holds every row of every versioned table at every commit, so
reading it is reading all of them, whatever SELECT and column privileges
say now or said when the rows were written: fetching a database is, for
its versioned schemas, what `pg_read_all_data` is. **Decided:**
predefined roles, as Postgres grants capabilities that bypass object
privileges.

- **Fetch and clone** from a database take its ownership or membership
  in `pg_prolly_fetch`, which the remote functions check (their EXECUTE
  revoked from PUBLIC as well).
- **Push** to it takes its ownership or membership in `pg_prolly_push`;
  and making or moving a branch's head there takes what doing it locally
  does (#124's checks).
- **A file remote** is the server's files: only superusers, or roles
  granted `pg_read_server_files` and `pg_write_server_files`, may use one,
  as `COPY … FROM 'file'` asks.
- Row security is no gap: a commit refuses row security policies on
  versioned tables, so none relies on them.

### 7.7 Durability and concurrency

- **One transaction.** A transfer's table files are added and its refs
  moved (every remote-tracking head a fetch moves, a push's head)
  together, WAL-logged as any chunk store change is (step 3): a crash
  leaves the transfer done or not at all; a standby replays it. Dolt
  moves each tracking ref separately, and keeps upstreams in a file
  outside the database (`repo_state.json`); here both are in the refs.
- **The WAL's cost.** A table file's bytes are WAL-logged before they
  are written (`table_file_write`), so a clone of B bytes writes about
  2B locally and sends B to each standby. That is what keeps standbys
  right without anything else; kept.
- **Concurrency.** Local commits during a fetch don't wait: a fetch
  moves only remote-tracking refs. Two pushes to one remote branch: the
  second's compare-and-swap fails, and it's refused with a hint to fetch
  first.
- **Resuming.** Not in step 6, as in Dolt (a failed push starts over).
  Room is left for it: received files stay out of the manifest until the
  end, so a later version can keep them across a failure and let the
  resumed walk count their chunks as received, checking them as 7.1
  does.

### 7.8 Building it

In order, each a PR with its tests:

1. The admission check (7.1) on every path that adds table files, with
   tests of each refusal: a dangling reference, a wrong hash, another
   format.
2. The transfer interface's implementation over a remote interface
   (file, then network): the walk with batched `has`, chunks sent as
   stored, received as table files; remotes and remote-tracking refs in
   the refs.
3. File remotes: fetch, push, pull, clone (whole files, then checked),
   with crash tests at each record and the shared-dependency check.
4. Network remotes over libpq (if (A)): the remote functions, their
   predefined roles, TLS tested; the transfer from known heads, with its
   fallback to the walk tested (decision 6).
5. V4 (`06-benchmarking.md`): clone, fetch and push between two local
   instances, timed and counted (chunks examined and moved against E and
   B; round trips against new commits).

Step 7 then pins chunks a transfer was told are present (7.1).

### 7.9 For the user to decide

Decided by the user on 2026-10-17, each as recommended:

1. **Network transport:** (A), SQL functions over libpq, behind the
   remote interface (7.5).
2. **Clone target:** (a), an existing empty database; a one-step clone is
   a client tool's (7.4).
3. **Push to a branch with uncommitted changes** on the remote: refused,
   unless `--force`, which resets them (7.3).
4. **Who may fetch or push:** the owner, or members of the predefined
   roles `pg_prolly_fetch` and `pg_prolly_push` (7.6).
5. **Object storage remotes:** later, as a transport over file remotes'
   layout (7.5).
6. **The transfer from known heads** (7.1) is built with the first
   network remote, not after: over a wide-area link round trips dominate
   a fetch of many commits (a thousand at 50 ms, 50 s), and the walk it
   falls back to is built anyway.

### 7.10 From Dolt

Taken: the closure invariant and its check before files are added; the
batched, overlapped walk that stops at chunks the receiver has; chunks
sent as stored, nearby ranges read as one; whole-file clone into an empty
store, with its fallback to the walk; received data as new files added at
once; a ref compared inside a compare-and-swap of the whole root; a push
refused over a dirty working set; and (for step 7) chunks pinned against
GC once a receiver says it has them.

Done otherwise: the check also on an empty receiver; chunks re-hashed on
receipt; files checked before they are renamed into place; one
transaction for all of a transfer's refs, upstreams in the refs; no root
overwritten by a clone; a bounded seen set; and the transfer from known
heads, one round trip for a line of new commits.


### 7.11 As built (file remotes)

Built: the remote interface with "file" (7.5), the transfer interface's
receive and send ("chunks"), remotes, remote-tracking branches and
upstreams in the refs (7.2), `dolt_remote`, `dolt_fetch`, `dolt_push`,
`dolt_pull` and `dolt_clone`, the views `dolt_remotes` and
`dolt_remote_branches`, and file remotes' privileges (7.6). The receive
check is 7.1's: each chunk re-hashed before anything reads it, a chunk the
remote lacks an error, each received commit's root of this build's
format, and only then one table file added. The typed walks stop at a
commit or root the visitor declines (#171), so a fetch or push reads what
changed, not the history.

Not as designed, or not yet:

- **The walk isn't batched or overlapped.** It asks the remote for a chunk
  at a time, as the walks read them; over a file remote that is a read.
  Batching (7.1) matters over the network and comes with it.
- **Clone walks** rather than copying the remote's files whole and then
  checking them (7.4): the same bound, O(B); the whole-file copy waits for
  V4 to say what it saves.
- **Shared dependencies.** A clone whose objects need something the
  database lacks fails where its copies are made, naming that object,
  and leaves nothing (one transaction). Naming every missing dependency
  before anything is written, as 7.4 asks, needs the folded root to record
  what its objects depend on outside the versioned schemas (types,
  functions, extensions, roles): items it lacks, so a format change, the
  user's to decide.
- **A push moves the remote's head as it is reached,** not with the
  transaction: the remote-tracking branch moves with the transaction, so a
  push whose transaction aborts leaves it behind the remote until the
  next fetch.
- **Not built:** network remotes over libpq, the transfer from known
  heads, `pg_prolly_fetch` and `pg_prolly_push` (they guard the remote
  functions, which come with them), deleting a remote's branch (so
  `dolt_fetch --prune` too), resuming, and V4.

## 8. Step 7 design: reclamation

**Status: decided by the user, 2026-10-17, as recommended** (8.9 lists
the decisions); not built.

Two stores reclaim space: the chunk store, whose history grows with every
commit and never shrinks today (a commit's changed chunks, every root the
refs had, what a deleted branch alone reached); and the page store, which
frees blocks already, as a fold lets go of old page versions
(`bmap_fold_prune`) and as a dropped relation's blocks lose their last
map, but leaks in three cases (8.6). Dolt's GC (2.4.2, `go/store/nbs`,
`go/store/types/value_store.go`, `go/libraries/doltcore/sqle/auto_gc.go`
and `dsess`) was read for this design; 8.8 lists what is taken from it.

The bound (`10-complexity-requirements.md`): space returns after a
branch is deleted and GC runs; a GC costs O(live chunks written since the
last GC) by default, O(live chunks) when full; neither stops commits for
longer than a short final window.

### 8.1 What is live

The roots are the refs tree at the store's root (refs `copies`): every
branch's head commit ('B'), its baseline and working-set root ('W'), its
merge in progress ('M'), and step 6's remotes and remote-tracking heads.
From a commit, its parents and its root; from a root, its trees.
Everything else in the store is garbage: earlier versions of the refs
tree (every ROOT record but the last), commits no ref reaches (a deleted
branch's, a forced reset's), and chunks a transaction wrote and then
didn't use (an aborted one's).

What isn't in the refs: chunks a running transaction holds only in
memory (a tree it is folding, a hash it read, a commit named by its hash
in a call). 8.3 keeps these without tracking them, by the transaction
horizon, where Dolt visits each session's roots (`VisitGCRoots`) and
waits for every session to reach a command boundary.

A commit no ref reaches is collected, so a call naming it by hash fails
after GC, as in Dolt. A reflog (Dolt's `dolt_reflog`, which its GC
truncates) would have to be a root, or say that it is lost.

### 8.2 The invariant

Step 6's closure invariant (a store that has a chunk has every chunk it
references) is what makes GC safe, so it is checked where chunks enter
the store from inside as well: when a transaction's chunks are appended
(each batch, and the rest at pre-commit), every reference they hold is
looked up, in the batch or the store's index. One that resolves in
neither is a reference to a chunk a GC collected meanwhile: the
transaction fails with a serialization failure (40001), retryable, never
a dangling tree. Dolt checks a commit's root's direct references
(`errorIfDangling`) and chunks a remote sends, not every local put; the
check here costs an index lookup per reference, which step 8 measures
against a fold's cost.

### 8.3 The collection, online

A GC runs in one database at a time, by `dolt_gc()` or the trigger
(8.5), and moves through four records:

1. **Seal.** `GC_BEGIN` (WAL) closes the journal: appends go on to a new
   journal segment (the manifest names segments, as it names table
   files). Everything in segments from there on is kept whole; the
   sealed segment and the table files are what the GC collects.
2. **Mark and copy.** From the root as of the seal, the walk marks what
   it reaches in the sealed files, breadth first, copying each live
   chunk as it goes into new table files (WAL-logged as any table file's
   bytes are, step 3). Writers go on: their chunks go to the new segment.
   Nothing blocks.
3. **Swap.** The journal's append lock is taken exclusively, which waits
   for an append in progress (one batch, not a transaction) and holds
   new ones off. Every chunk in the new segment is now a root too: the
   walk marks what they reference in the sealed files (all of what
   writers made meanwhile, committed or not, and so every hash they
   took from the sealed files and used), copying what it reaches. Then
   `GC_SWAP` (WAL) replaces the manifest whole: the new table files, the
   new segment, the next GC generation. The lock is released. The window
   costs the chunks written during the GC, not the store; the new files
   are checked (re-hashed, references resolved) before the window, not
   in it (Dolt checks its new files twice inside its blocking window).
4. **Retire.** The sealed segment and the replaced table files are
   unlinked once every transaction that was running at the swap has
   ended (the GC waits for their virtual transaction ids, as `CREATE
   INDEX CONCURRENTLY` does), so no backend reads a file it opened
   before (a VFD may be closed and reopened at any time, so an unlinked
   file isn't readable to the end of a transaction as an open one would
   be). `GC_RETIRE` (WAL) names them.

A transaction that straddles the swap and then appends a reference to a
chunk the GC found unreachable fails the check (8.2); one that only reads
reads on, in old files until it ends. A transfer (step 6) is a
transaction: chunks it was told the receiver has stay until it ends, and
its admission check runs against the store as it is then. A table file a
transfer or conjoin adds carries the GC generation it was made under, and
is refused (the transfer retried) if a swap changed it, as Dolt's
`ErrGCGenerationExpired`. Conjoin doesn't run during a GC.

### 8.4 Generations: the default and full

A GC's new files are closed under references (they hold everything the
live chunks in them reference, or a kept segment does). Recorded as such
in the manifest, they are **settled**: the default GC's walk stops at a
settled chunk without reading it, as Dolt's oldgen filter does, so a
default GC costs what changed since the last one, not the history. A
settled chunk that later becomes garbage (its branch deleted) is
collected only by a **full** GC (`dolt_gc('--full')`), which walks
everything and rewrites only the settled files whose garbage passes a
fraction (half), keeping the rest whole: a full GC of a mostly live store
writes little. Dolt keeps a second store for its old generation
(`GenerationalNBS`); here one manifest marks files settled.

**The mark's state is bounded.** A chunk's place is a file and an
ordinal in that file's index, so the marks are one bit per chunk of the
sealed files, not a set of hashes (Dolt's `visited` is a hash set of
every live chunk, unbounded); the walk's frontier is kept within
`maintenance_work_mem`, spilling as a sort does.

**The WAL's cost.** Copied chunks are WAL-logged: a default GC writes the
live part of what changed since the last GC, a full GC the live part of
the files it rewrites. A standby replays both and stays a copy without
anything else.

### 8.5 Starting it

- `dolt_gc()`, and `dolt_gc('--full')`: the database's owner, or members
  of `pg_maintain`, as `VACUUM` asks.
- **Automatic**, by autovacuum: a database's worker runs a default GC
  when its journal segment has passed `prolly.gc_journal_threshold` (128
  MB, Dolt's) and grown since the last GC by more than the settled files'
  size times `prolly.gc_growth_scale_factor`; a full GC when the
  settled files' estimated garbage passes a fraction, estimated from the
  branches and commits dropped since the last full one. Dolt's auto GC
  waits for a low load average; autovacuum's cost-based delay
  (`autovacuum_vacuum_cost_delay`) does that here.

### 8.6 The page store

The page store already frees blocks; three cases leak them:

- a crash between a drop's commit and the records that free its blocks
  (part 5);
- a block whose reference count saturated (255 maps), never freed;
- a crash part way through `bmap_fold_prune`, whose blocks stay
  allocated.

A **sweep** per tablespace and database finds them: every block-mapped
relation's map (every branch's copies, so every catalog row of every
branch) and every changed-page list's old versions are read, and each
physical block's references counted exactly; allocated blocks nobody
holds are freed, and saturated counts rewritten exactly (a count past
255 is kept elsewhere than the byte: a small table of blocks with large
counts, so a block shared by a thousand branches can be freed too). Only
making a branch adds references to blocks it didn't allocate, so the
sweep holds off branch creation in that database while it reads the
maps (O(map pages), not data pages); remaps and extensions allocate new
blocks, which the sweep never frees (it frees only blocks allocated when
it began). Free blocks at the page store's end shorten the file, as
VACUUM truncates a heap. VACUUM runs the sweep on a database when its
page store's allocated blocks exceed what its relations map by a
fraction; `prolly_page_sweep()` runs it by hand.

### 8.7 Crashes, standbys

- **Records.** `GC_BEGIN`, the table files' bytes, `GC_SWAP` and
  `GC_RETIRE` are WAL records, so redo makes the same GC on a standby and
  after a crash. A crash before the swap leaves the new files unnamed: at
  the end of recovery, files the manifest doesn't name, of ids the
  manifest has given, are unlinked (deterministically, from the
  manifest; not Dolt's prune, which deletes every file a process hasn't
  open, and so assumes one process owns the directory). The GC isn't
  resumed in step 7 (Dolt's resumes only its incremental oldgen files);
  it is run again, and its default walk is short where the last one's
  files settled.
- **Standbys.** Replaying `GC_RETIRE` conflicts with standby queries that
  began before the swap's replay (they may read the files): they are
  cancelled after `max_standby_streaming_delay`, as VACUUM's cleanup
  conflicts are (`ResolveRecoveryConflictWithVirtualXIDs`); a standby's
  feedback (`hot_standby_feedback`) doesn't hold a GC back, since chunks
  aren't xid-stamped.
- **Tests.** Crash tests at each record (before the seal's segment is
  written to, mid-copy, between the swap and the retire); a writer
  straddling the swap refused with 40001; a standby query cancelled by
  a retire; space returned after a branch is deleted and GC runs, and
  after the page sweep.

### 8.8 From Dolt

Taken: chunks written during the GC kept as roots (Dolt adds every hash a
session touches to a keep set, `gcAddChunk`; here the new segment holds
them); a short final window that blocks writes alone; copying live
chunks forward into new files, then one manifest swap carrying a GC
generation that table-file adds check; the journal retired at GC rather
than rewritten; generations, a default GC that stops at settled data and
a full one; conjoin held off during GC; an automatic GC on journal
growth.

Done otherwise: no per-read hook (Dolt's keeper on every get and has):
the transaction horizon keeps what readers hold, and the append check
(8.2) refuses what a writer would leave dangling; no session visits or
killed connections; the mark as one bit per chunk, not a hash set; new
files checked before the window, not in it; orphans removed from the
manifest's own record at the end of recovery, not by deleting what no
process has open; standby conflicts as VACUUM's; one manifest with
settled files, not a second store.

### 8.9 Decisions

Decided by the user on 2026-10-17, each as recommended:

1. **Online:** by seal, horizon and append check (8.2, 8.3), not a GC
   that holds every writer off for its run.
2. **The append check** on every batch of chunks, not only on a commit's
   root (8.2); step 8 measures its cost against a fold's.
3. **Generations:** a default GC that stops at settled files, and a full
   one (8.4).
4. **Automatic GC by autovacuum,** on by default (8.5).
5. **Unreachable commits** are collected at the next GC; a reflog, if
   wanted, is its own decision (8.1).
6. **The page sweep** holds off branch creation in its database while it
   reads the maps (8.6).

### 8.10 Building it

In order, each a PR with its tests:

1. The append check (8.2), with its test of a dangling reference refused.
2. Journal segments and the manifest's settled files.
3. The GC: seal, mark and copy, swap, retire, and the interface it is
   behind (a collector over the chunk store's primitives, so another can
   be swapped in); `dolt_gc()`; crash and standby tests.
4. The full GC and the rewrite threshold.
5. The page sweep and exact counts past 255.
6. Automatic GC and the page sweep under autovacuum.
7. Measured (step 8's suites): space after deleting branches and GC;
   default and full GC times against live and changed data; the window's
   length under a write load.
