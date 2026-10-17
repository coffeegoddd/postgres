# 11. Pages for the working set, Dolt-style history

**Status: steps 0–5 built and merged (2026-10-17); steps 6–8 not
started.** This replaces the design in which prolly trees are a table's
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
- **As built:**
  - A table without a primary key was read in full on any change
    (issue #126); its rows' copies are now counted from the changed pages
    as well.
  - `dolt_status` and `dolt_diff` failed on objects a commit can't hold
    yet (partitioned and foreign tables, aggregates, policies); reads now
    leave them out and only a commit refuses them (issue #130).

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
    the session on it, deleted once no session is on it.
  - A branch made from a commit has only its head until first used; its
    first use makes its copies. Making or deleting one takes what any
    branch does (CREATE, ownership, a transaction that can write); its
    first use takes neither, and a standby refuses it until the primary
    has used it.
  - A merge or cherry-pick that conflicts with uncommitted changes is
    refused, as Dolt does.

**Step 6. Remotes.**
- File remotes first, then a network protocol of our own: fetch, push,
  clone, pull; a cloned branch's copies made on first use from its head
  commit; the check for shared dependencies (part 1).
- **Exit:** V4.

**Step 7. Reclamation.**
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
