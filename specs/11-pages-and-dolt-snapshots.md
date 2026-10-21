# 11. Pages for the working set, Dolt-style history

**Status: steps 0–7 built and merged (2026-10-18), with the gaps each
step's notes list, and with them everything part 4's "Up to step 8"
lists; step 8 (measurement) not started. Decided by the user 2026-10-18
and not merged: writer modes (6.2, built in open pull requests) and the
Dolt test-parity program (part 4, after step 8).** This replaces the design in
which prolly trees are a table's primary storage (engines D1–D4,
`04-candidate-designs.md`). The decisions below marked "user" were made on
the dates given; everything else is a recommendation to be settled before
the step that needs it. Part 4 is the plan, with what each step built and
where the build departed from it.

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
  the commit itself. Built for a head-only branch's first use (#491): its
  copies are made from the branch nearest its head's root, ranked by an
  equal hash: one whose working set is known to be the root (#482; the
  copies are then the branch, nothing written or folded), one whose
  baseline's fold made it, one whose head did (only what that branch
  changed since is written back), else the session's branch. Dropping a
  branch's copies isn't built.

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

- **Hash:** SHA-256 (`hash_sha256.c`), as built; BLAKE3's first 20
  bytes (`hash_blake3.c`) since spec 22's group A.
- **Keys:** the memcmp-comparable key encoding from btree opclasses and
  collations (`keyenc_ordered.c`), as built; collation versions in the
  stored format stay a question (46).
- **Rows:** the canonical row encoding (`valenc_canonical.c`,
  `prolly_datum.h`), as built: types' binary send formats, portable
  arrays and composites, no local OIDs. Types without a binary send
  format are refused or stored as text (question 35; pending since step
  4h). Columns whose values are local OIDs (`oid`, `oid8`, `reg*`,
  `pg_node_tree`, a large object's `lo`, and the domains, arrays,
  composites and ranges made of them) are refused (#217). A column's value longer than 2 KB is
  a reference to a value tree of its own (#219,
  `14-large-values-and-toast.md`).
- **Chunker:** the byte-based rule already planned for step 5 (a key-only
  boundary hash salted by level, a Weibull rule around 4 KB, a minimum of
  512 bytes), with the maximum no longer tied to `BLCKSZ`: chunks live in
  the store's files, not in pages (question 45 goes away).
- **Node format:** `nodeformat_basic.c` loses its page header and becomes a
  variable-length chunk.
- **Large values:** value trees (#18), as built; addressed by byte counts
  since #218, and one per long column since #219
  (`14-large-values-and-toast.md`).
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
- **Changes to rows that change no page read the table again.** A
  baseline's tree is kept for the table's description and what its rows
  encode by without being rewritten: the columns' numbers (a column
  dropped and one of the same name and type added reads as NULL) and the
  definitions of their types (an enum's label renamed, a composite's
  attribute dropped), and the key encoder's, value encoder's and keying's
  implementations and format versions. Any of these changed, the next
  fold reads the table in full (#322, #323), unless the change leaves
  every row as it was: the baseline keeps the keying's row shape itself,
  and the keying tells (#485). So these keep the baseline:
  - an index, a constraint, a default, a trigger, a rule, a policy,
    reloptions, replica identity, statistics, storage, compression, a
    comment (none is in the shape);
  - `CLUSTER`, `VACUUM FULL`, `REPACK` (they rewrite no row's bytes, and
    clear only missing values the rows already hold);
  - a column added without a default (every old row has it as NULL,
    which isn't stored);
  - an enum's label added (`ALTER TYPE ... ADD VALUE`, `BEFORE` or
    `AFTER` too): rows send labels, the old ones keep their names, and
    their order, so keys too. A label renamed changes rows: read in full.

  And these change rows' bytes without writing them, so the baseline's
  rows are mapped to the shape now, read as they were folded and
  encoded as they are now, without reading the table (#485,
  `19-change-log.md` §2.2):
  - a column added with a default (rows from before read as its missing
    value, which every row's bytes now hold: no "lazy default");
  - a column dropped (gone from every row's bytes; a keyless table's
    rows sorted again, those become alike counted as one);
  - a column retyped where the type's cast keeps every value (no
    rewrite: a length or precision widened, `varchar` to `text`, a
    domain over the type), each value encoded as the new type's (#510);
  - a primary key added, dropped or moved: the rows sorted again by the
    key now, in bounded memory (#510).

  A retype whose cast computes (`int` to `bigint`, a `USING` expression)
  rewrites the table, which is read in full: what the rewrite did isn't
  recorded where the fold can read it (`19-change-log.md` §2.2).
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

**Folds skipped when nothing changed (#482; decided by the user
2026-10-20, in three phases: a generation per branch and a known working
set, then no fold around moves, then a generation per change log after
#481).** Status, diff and `dolt_branches` folded the same unchanged
working set again on every call. A fold is now skipped when nothing it
reads has changed since one made before (`refs_generations.c`, the
`copies` refs'):

- **A generation per branch**, in shared memory, keyed by (database,
  branch), with a count of the transactions writing the branch now. Each
  writer is told of before anyone else can see its write, hushed or not
  (the refs' optional `note_write()`, from `catalog/branch.c`): its rows
  (`branch_note_tuple()`, `branch_note_made()`), its versioned objects'
  catalog rows (DDL, `ALTER TYPE ... ADD VALUE`, `TRUNCATE`'s new
  storage, a branch's copies) and its versioned sequences' values
  (`nextval`, `setval`). When a writer's transaction ends (commits,
  aborts or is prepared), once others see it and its catalog changes are
  sent, the generation moves and the writer is remembered: the last eight
  by transaction id, the latest of those before.
- **An epoch** moves with every shared invalidation sent (any catalog
  change: a fold describes objects by what they depend on outside the
  branch too, such as a policy's role names) and every collection that
  dropped chunks (a fold's tree is no ref's, so garbage to it): the refs'
  `forget_folds()`.
- **Publishing.** A fold of a branch is stamped (generation, epoch) before
  it reads anything, and after it published if nothing ended, nobody
  writes the branch, the epoch stayed, and its snapshot sees every writer
  that ended (a commit landing between the snapshot and the stamp is one
  it doesn't). It becomes known when its transaction commits, if nothing
  ended meanwhile: its tree's chunks are others' only then, and gone if it
  aborts. Read folds, checkpoints', commits' and moves' folds publish; a
  read fold's says it left nothing out (others' don't say).
- **Taking.** A read of a branch's working set (status, `dolt_diff` from
  `WORKING`, `dolt_branches`' dirtiness, another branch's without checking
  it out) and `working_set()` take the root known if its baseline is the
  branch's now, nobody writes the branch, the epoch stayed, and the
  reader's snapshot sees every writer that ended. Nothing is published or
  taken by a transaction that wrote the branch or changed a catalog (its
  fold holds what nobody else sees), while any transaction is prepared
  (nothing is told of a prepared one's commit), in recovery (nor of a
  replayed one's), or with `debug_prolly_force_full_fold` or the fold
  verifier's folds. Nothing survives a restart.
- **No fold around moves.** A move (`move_settled()`: a hard reset, a
  fast-forward, a merge's or pick's move, a pull) takes the working set
  known instead of folding it, and returns at once if it is the target. A
  checkout compares the working set known of the branch it leaves with
  its head's and its baseline's root, and folds nothing if either; one not
  known is folded once, as the checkpoint, under the refs (it was folded
  twice, to compare and then to checkpoint), and the checkpoint kept even
  when clean, since its fold took its log's entries. Setting the baseline
  to a move's target instead of folding again after `move_to()` (when the
  log holds only the move's entries) isn't done: the next baseline holds
  each table's change finder state, which only the finder makes, so it
  waits for a generation per change log (#481).
- **Checked.** `debug_prolly_verify_fold_caches` folds again each working
  set taken and errors if the two differ; test_prolly's regression suite
  runs with it on (`test_prolly.conf`). `pg_stat_prolly` counts each one
  taken (`folds_reused`).
- **Known limits.** Any catalog change anywhere in the cluster (a
  temporary table, `ANALYZE`, autovacuum's statistics) moves the epoch,
  and any prepared transaction stops it, so neither is taken until the
  next fold. A long transaction holding back the xmin of every snapshot,
  with more than eight writers of a branch ended since it began, keeps
  folds of that branch from being published until it ends. A fold's
  tree is named only by the shared entry, so a collection forgets them
  all. Entries are 1024; a writer finding none free counts as a writer of
  every branch.

**Per-table work cached (#483; approved by the user 2026-10-20).** A fold
that does run still described, hashed and rebuilt trees for every
versioned table, changed or not, and a commit scanned all of `pg_depend`
for what the versioned objects depend on (1000 untouched tables: a
one-row commit 145–150 ms, a status after a write 61 ms, `--enable-cassert`
build). The canonicalizer (`canon_folded.c`) now keeps, in the backend and
private to it:

- **Each table's description** (and the hash its baseline is kept by, and
  its key's collations), by table, for the branch described on. A relcache
  invalidation of the table drops it; a syscache one of anything a
  description names drops them all (types, functions, schemas,
  collations, relations, columns, enum labels, roles, operators, operator
  classes, extended statistics: a trigger's function renamed sends its
  table nothing). Callbacks run for the transaction's own changes at each
  command's end, and at a cache reset (a branch switch). One described
  while an invalidation came isn't kept; all are dropped when the settings
  that change how a constant prints (`DateStyle`, `extra_float_digits`, …)
  change. `pg_stat_prolly.describe_cache_hits` counts each one taken.
- **The dependency tree**, against an epoch every callback moves.
  `pg_depend` and `pg_extension` have no syscache, and `ALTER EXTENSION
  ADD`, `DEPENDS ON EXTENSION` or an extension's version change alone
  sends nothing, so `inval.c` now sends a relcache invalidation of the
  catalog itself for each change to either.
- **Which objects are extensions' members** (one read of `pg_depend` by
  reference, not an index probe per relation, type and function) and
  which namespaces' objects are the canonical form's, against the same
  epoch.
- **The last canonical tree's items**: the next tree is it with only the
  items that differ applied (`apply()`), and the next baseline is the
  baseline folded from with the entries that differ applied.

A tree is taken only while its chunks are known to be there: made by this
transaction (not in a subtransaction rolled back) or one that committed,
and still in the store, which then keeps it from a collection (`keep()`).
`debug_prolly_verify_fold_caches` works out again whatever is taken and
errors if it differs (test_prolly's suite runs with it on). After: a
one-row commit with 1000 untouched tables 19 ms, a status after a write
16 ms. What is left per table is mostly the change finder's
(`changes_log.c`: about 8 of the 16 ms), and the `pg_class` scan, left as
it is; caching the finder's handle (its keying, key and equality
procedures) is the change log's to do. The caches are a backend's: a session's first fold fills them.

**Moves share another branch's pages (#491; decided by the user
2026-10-20: O(Δ) with small constants, not O(1); no hierarchical block
map).** A move (a hard reset, a fast-forward, a merge's or pick's move, a
first use) to a root another branch's working set is known to be (part 2's
known working set: nobody writing it, every writer that ended seen) gives
each table whose rows differ that branch's storage instead of writing the
rows (`share_clean_tables()`, `refs_copies.c`): the table's, its indexes',
its TOAST table's and its change log's, each a share of the other
branch's (`RelationCreateStorageShared()`, with the visibility and free
space maps copied, #487), through the branch operations' optional
`share()` (`branch_copies.c`). The baseline then takes the other branch's
entry for the table (the canonicalizer's optional `splice_baseline()`),
which its shared log goes on from, and the working set is folded again,
so nothing is read in full. Chosen by cost: a table is shared when its
blocks (its relations', about 10 µs each to share) cost less than its
changed rows (about 11 µs each to write), counted by a diff of the two
data trees. Only for a move of rows (the two roots define the same
objects), only tables whose relations are the other branch's too (the same
ids: made from the same), and only where the locks are free without
waiting: the moved branch's copies as `TRUNCATE` locks them, the other
branch's as a copy does (against its writers), and the other branch still
known at the root once they are held. As `TRUNCATE`, it isn't MVCC-safe:
a snapshot taken before the move commits reads the shared storage after.
A first use whose source's working set is known to be its head does
neither: the copies are already the head. The known working set is
fragile (any catalog change anywhere moves part 2's epoch), so a move
shares only where a fold of the other branch since the last catalog change
made it known. A hierarchical block map, for O(1) moves, isn't built.

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
(Deferred, user 2026-10-17: only smgr is one; the hooks below are this
design's and built regardless.) What this design needs from each:

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

As of 2026-10-17, steps 0–5 below are built and merged, and step 6's
file remotes; each step says with which PRs. D1 is removed by #177, #178
and #183: the "Today" column below is its history, at `cff08be5c3`, its
last state.

### What carries over

| Today | In the new design |
|---|---|
| `dolt_*` functions, views, `dolt.author`, `db/branch` connections, conflicts as in Dolt (#25, #26) | Kept; their backend changes to per-branch copies and the chunk store |
| `CREATE DATABASE … VERSIONED` and its refusal of heap tables (#27) | Superseded: version control is per schema (versioned schemas), so any database can hold both. Removed with D1 (#178, #183) |
| Hash, key encoding, canonical rows, value trees, canonicalizer (4a, #18, #23) | Kept: the chunk store's contents |
| Copy-on-write trees, subtree-skipping diff, three-way merge, merge conflicts, refs, commits (4c, #9, #21, #22, #26) | Kept; they operate on chunks from the new store instead of node-store pages |
| Chunker, node format | Reworked: the byte-based rule (step 5's plan) and pageless nodes |
| Interface registry and stub pattern (`prolly_registry.c`) | Kept; the new interfaces register there |
| Requirements suite (`src/test/prolly`), `test_prolly`'s version-control tests, the benchmark harness, gates 1–7 | Kept; storage-engine tests went with D1 (#178), what only they covered ported first (#177) |
| The node store (`nodestore_global.c`), reclamation (#19) | Replaced by the journal and table-file store and its GC; removed with D1 (#183) |
| D1 and K1 (table AM on trees, row locks, TID maps, keyings, spill, SERIALIZABLE at table granularity, the 4h fixes to them), the prolly index AM | Superseded by pages. Removed by #177, #178 and #183, ahead of step 8, which builds them from `cff08be5c3`, D1's last state |
| `09-versioned-catalogs.md` part 4 stages 2–7, and #34 (catalog writes through the table AM) | Superseded by per-branch copies of versioned schemas; #34 reverted with D1 (#183) |

### Steps

Each step is a stack of PRs with its own tests (every bug with a test that
fails without its fix), and Postgres's suites keep passing after each one.

**Step 0. Settle the open stack and these specs.** Done 2026-10-15.
- #12–#47 merged; the D1 engine's state then is tagged `prolly-d1`. (Its
  last state, after #53's rename of the index AM and three later fixes,
  is `cff08be5c3`.)
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
  AM, which holds the name `prolly`, becomes `prolly_d1_index` (removed
  with D1, #183).
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
  - (Since #249, not merged.) A commit is refused as empty before
    anything is recorded; the refused fold still advances the branch's
    baseline, since what it read is the head's. A change of an
    extension's version alone is not a change: the dependencies are
    beside the root, not in it (part 7.4), so `dolt_commit` refuses it as
    nothing to commit.
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
  folds that table alone. (Of those, only foreign tables remain: the
  objects stack, below.)
- **As built (objects):** commits hold aggregates, row security policies
  and partitioned tables (the objects stack, #205–#208); since, range
  types (#230), and extended statistics, casts and collations of
  versioned schemas (#231). Still refused: foreign tables, window
  functions (only C writes one), base types, partitions apart from their
  partitioned tables (below), range types with a canonical function or
  whose subtype difference function, operator class or collation is of a
  versioned schema; and (#231) every other kind a branch-aware catalog
  holds in a versioned schema, which were neither held nor refused
  before: operators, operator classes and families, conversions, text
  search configurations, dictionaries, parsers and templates. That list
  is the branch-aware catalogs with a schema column whose kind a commit
  doesn't hold, so a kind added later is refused, not left out. Each is
  listed as not covered, named as Postgres describes it.
  - **Only a commit refuses** (#231). A move, a checkout and a checkpoint
    fold the working set leaving such objects out: a move to a commit (a
    reset, a branch's first use, a push's) discards them and what depends
    on them as uncommitted work; a checkout leaves them on their branch,
    whose catalogs hold them; a push is refused over them, as over any
    uncommitted change. (Before, any move refused while one existed, and
    a reset couldn't discard it.)
  - Extended statistics are part of their table's definition (a section,
    if it has any: each one's name, its CREATE STATISTICS and its target);
    a cast of a versioned type that isn't part of a type is an "x:" item
    (its method and function, its context); a collation of a versioned
    schema is an "l:" item (provider, locale, rules, determinism). Moves
    make and drop them; a collation, like a range type, can't be altered,
    so a changed one nothing uses goes and comes again and one in use is
    refused.
  - A range type is a type item, "range", with its subtype, the
    subtype's operator class, its collation, its subtype difference
    function and its multirange type, as CREATE TYPE ... AS RANGE names
    them; its multirange type, constructors and cast come with it (and
    aren't items). `pg_range` and `pg_cast` are branch-aware catalogs
    (part 6.3): a range's row belongs where its type does, a cast where a
    versioned type it casts does. A move makes one and drops one; one
    defined otherwise goes and comes again if nothing uses it, and is
    refused if something does (or if a type it uses changes), as Postgres
    can't change one in place. Operator classes' entries (`pg_amop`,
    `pg_amproc`) count as versioned where their family is, so a class can
    be made in a versioned schema (and is refused by a commit).
  - An aggregate is a routine item whose definition is the CREATE OR
    REPLACE AGGREGATE that makes it, every option written (Postgres has
    no deparser for aggregates; pg_dump writes them so). A move replaces
    one changed in place (what uses it, a materialized view, stays),
    unless a function is dropped: then every aggregate goes before the
    functions and comes after them and after new tables, whose row types
    they may use. A removed one goes before anything else.
  - A table's definition has a policies section after its rules (before
    a partitioned table's or partition's sections), if it has any policy
    or row security on: row security on, and forced, then each policy as CREATE
    POLICY makes it, its roles by name (a table with neither has no
    section, its description as older builds wrote it). Roles aren't
    recorded dependencies (they aren't versioned): a policy naming a role
    the database lacks fails where it is made, and a role renamed changes
    the text of every policy naming it, as it changes what pg_dump
    writes. A move drops every policy first and makes every one last, as
    views go and come, as a policy may use anything the move changes.
    The fold and the row writer work below the planner, where policies
    don't apply. `dolt_diff`, `dolt_conflicts` and
    `dolt_constraint_violations` refuse a table whose row security
    applies to the reader (part 4, item 5).
  - A partitioned table is a table item with an empty data tree and its
    key; a partition, a table item with its parent and bound, its rows
    folded as any table's. A partition's description holds only what is
    its own: not the constraints it has of its parent (keys, foreign
    keys, checks, not-nulls), the indexes attached to its parent's, or
    the triggers cloned from its parent's, which Postgres makes with it
    and names as it names them (so an attached index's own name isn't
    held). A move makes partitioned tables before their partitions (by
    PARTITION OF, then the partition's own defaults), drops partitions
    first, and detaches (first) and attaches (after the rows) a partition
    whose bound or parent changed. Refused at a commit: a partition with
    its columns in another order than its parent's (PARTITION OF would
    make them in the parent's), a partition and its parent split between
    versioned and ordinary schemas. Refused at a move: a changed
    partition key, a table becoming or ceasing to be a partition (made
    again or not), a partitioned table whose columns would be made
    again.
  - Names are read as identifiers where a move drops by name (a quoted
    trigger, rule, index or policy name with a space came back cut).
- **As built:** a table's schema, a type's definition and a collation's
  description are text split into lines and fields, and the names and
  expressions in them were written as they were: a quoted name or string
  literal with a tab or newline (and every rule's definition, which
  Postgres prints over several lines) was read back wrongly by a move or
  merge, or crashed it. Since format 3 of the root's `"!:format"` item
  (`FOLDED_FORMAT`; not the canonicalizer's `format_version`, a
  separate number) every such field is escaped (a backslash, tab or
  newline as `\\`, `\t`, `\n`) and read back where it is used (#195);
  functions', views' and materialized views' definitions, never split,
  are kept as they are. A sequence's definition is split too, but holds
  only its type (one of three built-in names) and numbers: nothing to
  escape, and a field added that could hold a name must be. A clean
  break, as format 2 was (user, 2026-10-17, choosing escaping over
  refusing such names, which are legal Postgres): an older root's rows
  aren't read, and it is neither moved to nor merged, a merge base
  included ("commit the branch" to write it anew); between an older root
  and a current one, `dolt_diff` and `dolt_status` may list an object
  whose definition holds a backslash, tab or newline as modified, as its
  text was re-escaped. `dolt_status` splits its rows at the last tab and
  sorts them by name, a table's name holding a tab included.

- **Table settings (#400, folded format 10):** explicitly set table and
  TOAST reloptions are versioned, sorted by option name, including explicit
  values equal to built-in defaults. Resetting an option removes it so the
  server's policy applies again. These tuning choices (fillfactor,
  autovacuum, parallel workers, and other accepted reloptions) travel with
  the schema; tablespaces and physical placement remain local. Column
  storage differing from its type's default, explicit compression methods,
  and explicit statistics targets are versioned too; default storage,
  compression and statistics have no extra fields. Replica identity is
  versioned by mode and, for USING INDEX, the index's name; an index mode
  whose index was dropped is described as NOTHING, its effective behavior.
  TOAST options are described only when the table needs a TOAST table
  (one left over from dropped columns isn't made again with the table, so
  its options couldn't be either). Materialized views' options and column
  settings are versioned the same way, as lines of their description.
  Replay applies column settings without recursing into children and sets
  replica identity after creating its index. Options merge by name, column
  settings with their column, and replica identity as one setting; an identity by an index the merge
  doesn't keep is a schema conflict. Prolly
  tables cannot be unlogged, so persistence adds no field. Format 9 omitted
  these settings; format 10 is a clean break, with the same refusal of
  older roots as previous format changes.

- Format 11 of the folded root orders a primary key whose opfamily registers its
  comparison for a type the key's type is binary-coercible to by that
  comparison, as PostgreSQL's opclass input types do: `varchar` (and
  domains over it) through `text`, `cidr` through `inet`. Previously those
  keys fell back to content-keyed rows with a copy count, so a merge
  across `char` to `varchar`, or `inet` to `cidr`, read the wrong row
  format (#389). The ordered key encoder is format 4, which also invalidates fold
  baselines. Older roots are refused for row reads, moves and merges;
  commit a branch to write its current rows in the new format. A merge
  whose base was committed in an older format stays refused, as with
  every earlier format change.

**Step 5. Moving heads.**
- The row applier (from logical replication's apply worker); checkout and
  reset to a commit, fast-forward, branch at a commit, materializing a
  branch with no pages.
- Three-way merge over trees into a working set with changes; conflicts
  as built; cherry-pick, revert.
- Decided (user, 2026-10-16):
  - Reset, merge and checkout of a commit on a branch other sessions
    use behave as in Dolt: the branch's working set changes under them,
    as a committed transaction's changes do. (In the writer modes other
    than `shared`, 2026-10-18: they need the branch's ownership, and on
    a protected branch they are integration operations; 6.2.)
  - Rows written by merge, reset and fast-forward fire no user triggers
    and recheck no foreign keys, as logical replication's apply does;
    merge records constraint violations as `dolt_constraint_violations`
    rows, as Dolt does.
  - Schema merge follows Dolt: additive changes merge, the same object
    changed differently on both sides is a schema conflict, and a
    conflicting column type change fails the merge.  (User, 2026-10-18,
    #159: as Dolt's, a column one side changed type merges with the other
    side's changes to the table only as a string type (varchar, char,
    text) made no shorter; any other change of type, int to bigint
    included, is a schema conflict.  A table the other side didn't change
    takes the change whatever it is.)
  - Both sides retyping a column to the same different type can leave an
    ambiguous base (#391): its text form read as the new type and its SQL
    cast need not agree, and history does not record USING expressions.
    A non-key value with two successful but different readings is treated
    as changed on both sides, conflicting if the sides differ. For keys
    and keyless rows, compare the outcomes under both readings before
    applying rows. Keep a text-reading conflict for resolution; otherwise
    refuse if the rows, copy counts, or conflict status differ. Matching
    outcomes still merge. This prevents an edit to the old text form or a
    deletion from silently disappearing without rejecting insert-only
    merges whose outcomes agree. Failed text readings retain the existing
    uncertainty/refusal rules; ordinary diffs still show historical values.
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
    that may not stays until one that may checks out. No session moves
    a hidden branch: `dolt_checkout` and a `dbname/branch` connection
    refuse its name, `dolt_branch -m` doesn't rename it, a push neither
    sends one (`dolt_push`) nor moves or deletes one
    (`dolt_serve_move_head`, as `dolt_serve_heads` doesn't list them),
    and `dolt_clone` makes no branch of a remote's branch so named (a
    file remote may hold one), only tracks it.
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
    sequence or materialized view that has uncommitted changes. When
    definitions change, uncommitted or on either side of the merge, the
    uncommitted changes are set aside (#494): the branch is moved to its
    head, writing only what they changed; the merge is made and
    committed as over a clean working set; then each canonical item they
    changed is put back over the result, as the working set had it, and
    the branch moved there, writing only those. Refused, the
    transaction's error undoing the merge, where the merge changed an
    item they changed too (a table, sequence, view, type, ...), where a
    table they changed uses a type the merge changed, where their
    definitions don't fit the merged ones, and where a foreign key from
    or to a table they changed is broken. The put-back is an item-level
    apply, not the canonicalizer's splice(): a splice brings a table's
    sequences with it, which would undo the merge's advance of a
    sequence another table shares.
  - A materialized view's rows merge as a REFRESH made them: those of
    the side that refreshed it since the merge base (ours if neither), or
    of the side whose query the merged one is; if both refreshed it to
    other rows, neither's are what its query reads after the merge, and
    it is refreshed from the merged rows (refused over uncommitted rows,
    which it would read). A fast-forward over uncommitted changes brings
    theirs' rows too (#320).
  - A stash's pop or apply is the exception to the refusal by table: as
    Dolt's (`MergeRoots(working, stash, parent)`), it merges three-way
    into the working set as it is, the uncommitted changes ours and the
    stash's parent the base, so it is refused, the stash kept, only where
    a row both changed conflicts, or where definitions change as above
    (#585). A pick or revert can't: its commit leaves the uncommitted
    changes out, so it mustn't merge with them.
  - **Bulk apply (#490, built 2026-10-20).** The row applier gained an
    optional `insert_many(a, rows, n)` (NULL: `insert()` each). A move
    (reset, merge, cherry-pick, revert, pull, checkout, clone) holds the
    rows it inserts into a table, up to 1000 rows or 1 MB, and writes them
    as COPY does: `heap_multi_insert` with a bulk-insert state (its pin
    let go after each batch, so nothing stays pinned between writes),
    then each row's index entries and queued rechecks. Each row's change
    log entry is still written (`note_made()` takes a page's at once), as
    are the branch's notes. A merge that records constraint violations
    still writes each row alone, in its subtransaction.
    - A table filled from nothing that is new in the transaction (made
      by the move, a clone's tables, or given new storage under ACCESS
      EXCLUSIVE) is a load: at least 100 rows in the first batch, no
      deferrable unique or exclusion constraint (whose rechecks are
      queued per row), not a materialized view. Its rows get no index
      entries as they are written; its indexes are built over them
      (`reindex_relation`, unique and exclusion constraints checked
      then, the primary key not re-checking key lengths) before any
      other write or at the applier's end. No other transaction sees
      such a table, so nothing reads the indexes meanwhile. A table
      emptied by TRUNCATE earlier in the move's transaction is loaded
      too (its indexes, already given new storage by the TRUNCATE, built
      again); `bulk_moves` and `t/151` (crash, `wal_level` minimal and
      replica) cover it.
    - A load's end can fail (the build finds a duplicate key). Its
      failure ends the applier, even if the caller catches the error:
      every later write but `end()` raises, since the indexes are closed
      or, after a subtransaction's rollback, back to their storage from
      before the build, with no entries for the rows loaded. The build
      is refused in a subtransaction begun since the rows were loaded
      (rolled back, it would leave them unindexed), which also ends the
      applier; the indexes are closed and opened again under the
      resource owner of the applier's `begin()`. No caller reaches
      either today: the only one that writes in subtransactions (a merge
      recording violations) doesn't batch. `applier.sql` checks both
      from SQL (`test_apply_failed_load()`).
    - User triggers: none fire, as before (the applier keeps only the
      deferrable constraints' rechecks); no foreign key is checked.
    - Not built: deferring index upkeep on a table other sessions can
      write (its indexes marked not ready and rebuilt) needs a lock the
      move doesn't hold, and REINDEX's ShareLock taken under the move's
      ROW EXCLUSIVE could deadlock with another writer. A keyless table's
      row hash is already built lazily (after 8 lookups, and only by
      removals or updates), so a move that only inserts never scans the
      table. Skipping the change log during a clone's load (a new
      table's storage is folded in full anyway) belongs to the change
      log's work. Parallel decoding waits for a profile.
    - Measured (100k and 1M rows of `(int pk, md5 text, int indexed)`,
      `wal_level = replica`, a checkpoint before each; times on a loaded
      machine, other builds running):

      | Move | Before WAL / time | After WAL / time |
      |---|---|---|
      | reset inserting 1M rows into the table | 569 MB / 7.2 s | 463 MB / 6.1 s |
      | reset making a table of 1M rows (load) | 334 MB / 4.2 s | 181 MB / 3.0 s |
      | reset across a column type change, 1M rows | 878 MB / 12.7 s | 666 MB / 10.1 s |
      | COPY into the indexed table (reference) | 189 MB / 2.2–4.0 s | |
      | COPY, then the indexes built (reference) | 79 MB / 0.8 s | |

      At 100k rows the reset into the table wrote 46.6 MB; by record:
      heap multi-inserts 9.7 MB and btree inserts into the two indexes
      14.4 MB (together 1.24× the COPY's 19.0 MB); the rest is pruning
      and hint-bit page images of the pages the previous reset emptied
      (17.5 + 16.6 MB) and 100k heap deletes (5.4 MB), none of them the
      apply's. Before, the same apply wrote a heap insert per row. The 5× speedup the issue
      asked for at 1M rows isn't reached: the time left is the per-row
      index upkeep on a live table (not deferred, above), decoding, and
      the change log; a load's extra WAL over COPY-then-index is its
      change log entries.

**Step 6. Remotes.** Designed in part 7 (decided 2026-10-17). File remotes
built (#171, #172, #174, #175; 7.11); network remotes, batching, the
transfer from known heads and V4 built and merged (#198–#203, 7.12).
- File remotes first, then network remotes over libpq: fetch, push,
  clone, pull; a cloned branch's copies made on first use from its head
  commit; the check for shared dependencies (part 1, built: 7.4).
- **Exit:** V4.

**Step 7. Reclamation.** Designed in part 8 (decided 2026-10-17). Built
and merged (#210, #211, #212, #213, #214, #216, #223, #225; 2026-10-18);
8.11 says what was built and where it departs from part 8. The eviction
self-deadlock under a map page's lock, which its crash tests (and CI's
run of `t/041`) hit, is fixed at its cause in #227 (8.11).
- Chunk-store GC with WAL (table files, `oldgen`, journal retirement);
  page-set and physical-page reclamation; the fold epoch's retained page
  versions.
- **Exit:** space returns after branch deletion and GC; crash tests of GC.
  Met: a full collection gives deleted branches' space back (8.11's
  measurements), and the collection's and the sweep's crash and standby
  tests pass.

**Up to step 8 (user, 2026-10-17).** Decided as recommended, each group
a stack of PRs, in this order. **All built and merged (2026-10-18):** the
dependency check (#188, #189, #192, #193), escaping (#194–#196), network
remotes (#198–#203), reclamation (#210–#214, #216, #223, #225), the
objects (#205–#208), large values' refusal and B (#217–#221, #224); range
types and what commits hold or refuse since (#230, #231) are in the same
stack as this note.
1. **The dependency check at moves** (part 7.4): checkout, merge, reset,
   cherry-pick and pull refuse a commit needing what the database lacks,
   naming all of it; a move never creates or alters an extension. (The
   dependencies are in the commit, in a tree of their own whose values
   are `text[]` literals, not items of the root: the escaping below
   doesn't touch them.)
2. **Names escaped in the canonical encoding.** The canonical text joins
   fields with tabs and lines with newlines, and a name may hold either
   (a type `"tab<TAB>in"` breaks a column's definition today). Every name
   and other free text in an item is escaped; a clean break of format, as
   #149's: a root records its format, and an older root's items aren't
   read. Refusing such names was the alternative: they are legal
   Postgres. Every format change before step 8 is such a clean break
   (B's too, item 6): no history yet is worth keeping readable across one,
   and keeping it would mean reading every older format; once history is
   kept (step 8's measurements done, users), a change keeps older roots
   readable, as spec 14's B1 first proposed.
3. **Network remotes** (7.8, items 4 and 5).
4. **Reclamation** (step 7, 8.10).
5. **Objects a commit refuses today:** aggregates, policies (row-level
   security, with the table's `relrowsecurity`) and partitioned tables
   (their partitions, bounds and keys) get canonical forms, moves and
   merges. Foreign tables stay refused: their rows aren't the database's,
   and a definition without its server would be a commit no other
   database could check out. A partitioned table with a foreign table
   among its partitions is refused with it (no partition is left out).
   Commits holding policies changes 7.6's "row security is no gap":
   history holds every row whatever the policies say, so fetching keeps
   its roles (bypassing row security as BYPASSRLS does, further than
   `pg_read_all_data`, which policies still filter), and the
   readers of history in SQL (`dolt_diff`) refuse a table with row
   security that applies to the reader, rather than show it rows the
   policies hide.
6. **Large values** (`14-large-values-and-toast.md`): the refusal of
   `oid` columns, then B (B2, B1, B3, B4). C waits for step 8's
   measurements and its own gates; for C, jsonb's canonical bytes are its
   on-disk format with a version byte, and the pointer tags its hash
   algorithm. B adds references reclamation must follow (B1's column
   value trees): B keeps 8.1's liveness whole, every reference a chunk
   holds findable from it, and B3's memo is no root (a hit is checked
   against the store, so a value reclamation took is read again).

The interfaces under the pages stay deferred (only smgr is one): making
the facilities part 3 lists swappable is deferred, not the hooks this
design needs in them (the buffer manager's remap, part 3 and 5, is built).
The correctness suite joins CI only once its baseline is clean.

**Step 8. Measure.**
- Gates 1–7 and the S and V suites for this design against stock heap and
  against D1, built from `cff08be5c3`, its last state; `10-complexity-requirements.md`
  rechecked operation by operation.
- Before it: the engine's work counters (`pg_stat_prolly`) and the
  fixed-work run shape (#241, #242; `17-performance-on-prs.md`), which
  the measurements read.

**Dolt test parity (user, 2026-10-18, as recommended; #153).** Prolly's
version-control tests are brought to parity with Dolt's: every Dolt
behavior in a feature prolly has gets a test on prolly, and every
intended difference is an entry in `18-dolt-divergences.md`. Scope
(#164): every Dolt version-control feature but the staging area.
- **Where it stands.** `vcscenario` declares `vc.core` for `prolly` and
  CI runs it; three of the four reviewed ports pass, and the fourth
  (`dolt-reset-identity`) is deferred as a divergence (sequences are per
  branch). Its operations are commit, branch, checkout and hard reset,
  with sessions and expected SQLSTATEs.
- **Ported now:** the Dolt cases prolly can run today, and with them
  these small features, each built with its cases:
  `dolt_revert('--abort' | '--continue')`; `dolt_merge('--ff-only' |
  '--squash' | '--no-commit')`; `dolt_branch('-m')`; `dolt_merge_base()`
  in SQL (the merge base is computed internally today,
  `copies_merge_base()`); `dolt_verify_constraints()` for unique, CHECK
  and NOT NULL constraints (foreign keys only today). Large features
  (diff system tables, `dolt_history_<t>`, tags, `AS OF`, rebase, stash,
  …) become issues of their own later, each porting its own cases (#163's
  policy).
- **How:** the pinned catalog of Dolt's cases gets a classification, P
  (portable), F (needs a named feature) or N (not applicable), and a
  coverage report (#157); ports are written by hand, with a generator
  only if hand porting proves slow.
- **Formats.** Dolt's engine ScriptTests become `vcscenario` JSON; Dolt's
  Go truth tables (#161) and Doltgres's tests (#162), SQL regression
  tests; blocking and serialization between sessions (#165), isolation
  specs; sequential interleavings, multi-session scenarios. Typed
  operations only where the adapter must capture hashes or state;
  `dolt_*` calls are allowed in query steps that declare the capability
  they `require`. A case needing a missing feature stays unported,
  marked F with the feature's name; partial ports are allowed with a
  review note. Dolt's Go tests and Doltgres (`d5e05f46`) are pinned in
  `source-lock.json`.
- **Divergences found while porting** are fixed to match Dolt unless
  Postgres semantics force the difference; then they are recorded in
  spec 18 with a stable ID that `ports.json` cites. The user is asked
  only when a decided spec conflicts.
- **CI:** `prolly.yml`'s `vcscenario` step now, spec 16's gate later;
  version-control operations in the differential runner (#166) nightly,
  with a fixed set of seeds on every pull request, checked against
  invariants and a reference model (Doltgres only by hand).

**D1's removal, moved ahead (user, 2026-10-17).** D1 is retired once file
remotes (#171, #172, #174, #175) merge, before step 6's network remotes,
not after step 8's measurements: those build D1 from its last state,
`cff08be5c3`, instead. In
order, each a PR:
1. **Coverage first.** What only D1's tests cover is tested on versioned
   schemas (`dolt_*` functions' arguments, errors and views, conflicts and
   their resolution, branches and merges across crashes and standbys:
   `t/019`–`t/023`); and whether `src/test/prolly`'s run of the core
   regression suite with prolly as the default table AM (`prolly_am.conf`)
   runs on D1 is checked, and if so replaced for this design. **Done
   (#177):** the behaviors are tested on versioned schemas, and
   `src/test/prolly` defaults to `prolly`.
2. **D1's SQL surface:** the `prolly_*` version-control functions,
   `CREATE DATABASE … VERSIONED`, the `prolly_d1` table AM and the prolly
   index AM, `prolly_gc()`. **Done (#178, #183):** D1's tests, harness
   targets and tool adapters went first (#178), its SQL surface with its
   code (#183).
3. **D1's implementations:** the engines (`d1`; `d2`–`d4`, stubs),
   coordination, the node store, root layout, row identity, the commit
   pipeline, the log, reclamation's `mark_sweep`, refs `tree`, diff-merge
   `rows`, canonicalizer `catalogs`, value encoder `heaptuple`, keying
   `pk_or_content`, chunker `keyhash`; configuration down to one profile.
   **Interfaces no remaining component uses are deleted with them**
   (engine, coordination, node store, root layout, row identity, commit
   pipeline, log, and any other left with only a stub); every interface
   this design uses stays, reclamation's among them (step 7 implements it
   again). **Done (#183):** the interfaces kept and removed are listed in
   `00-architecture-principles.md`; their numbers are kept, with gaps.
   Removing D1 also removed its WAL resource managers (Prolly,
   ProllyStore, ProllyCommit; ProllyChunks stays), lock tags, settings,
   two-phase-commit hooks and `pg_database.datversioned`, and the core
   patches only it used: `GetOpenSubTransactionIds()`, the table AM row
   sampler for ANALYZE, and optional parallel scans
   (`AMFLAG_NO_PARALLEL_SCAN`); `02-postgres-integration.md` lists which
   of its section B patches are still live.
4. **The specs:** D1's columns and rows become pointers to history.
   **Done:** specs 00, 02, 04, 07, 08, 10, 12, the README and this part
   point to D1's last state.

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
  primary's allocations for them, so they aren't shared. A branch's copy
  (step 2) gets copies of its source's (full-page images in the WAL): its
  pages are the source's byte for byte, so what the maps say of them
  holds, and changes after clear its own bits as any change does. An
  empty map would be correct too, but every index-only scan would visit
  the heap, and VACUUM every page, until the branch was vacuumed (#487).
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
access method, which held the name, became `prolly_d1_index` (removed with
D1, #183).

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
- Incremental backups send the page store in full (#334), so on a
  block-mapped cluster an incremental backup is about as large as a full
  one: WAL names a relation's logical blocks, never the page store's
  physical ones, so WAL summaries have no entry for it, and an incremental
  copy would send none of its changes. Its allocator, reference counts
  and the map and changes forks are summarized and sent incrementally.
  `pg_combinebackup` refuses a page store sent incrementally. Summarizing
  the physical blocks (from the maps, at backup time) would make the
  store incremental too.
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

- **`pg_branch`**, a database-local catalog, lists the branches that have
  copies: an OID (the branch id used in catalog rows) and a name. `main`
  is a bootstrap row with a fixed OID. Branch id 0 means "shared": no
  branch. Heads, baselines and working-set roots are in the refs (step
  4). A branch gets its row at its first use (`materialize()`,
  `refs_copies.c:606-666`); one made by `dolt_branch`, a push or a fetch
  has only its head in the refs until then (step 5's head-only
  branches).
- A session's branch, `MyBranch`, is `main` unless the connection named
  another (`dbname/branch`). `dolt_checkout()` changes it and resets the
  backend's caches as an invalidation overflow does.
- A session on a branch holds a session-level lock on it, which stops its
  deletion (`check_out_branch()`, `branch.c:743-780`; the user lock
  method, not taken for `main`).

**Writer modes (decided by the user 2026-10-18, as recommended; #148).**
A branch's working set is shared: every session on the branch writes
into it, and `dolt_commit` commits all of it, its own changes and every
other session's SQL-committed ones. That is Dolt's model, and it stays
the default. Writer modes make one writer per working set an invariant
when a database asks for it, so a commit is one writer's work, and can
keep `main` changed only by merges. Built in pull requests not yet
merged (#251, #256, #262, #265); "As built" at the end of this part says
where they depart from it.

| Mode | `main` | Other branches | For |
|---|---|---|---|
| `shared` (default) | writable, shared | shared | applications using the database as an ordinary OLTP database; Dolt's semantics, today's behavior |
| `owned` | writable | one writer per dirty working set | small teams and scripts: attributable commits without branching |
| `branch-only` | protected: writes refused, with a hint to `dolt_checkout('-b', …)` | one writer each | reviewed changes: branch, commit, merge |
| `branch-auto` | protected: a write moves the session to its writer branch | one writer each | agents and many independent writers who shouldn't manage branches |

- **The mode** is stored per database (a setting in the refs) and
  changed by `dolt_writer_mode(mode)`, which takes owning the database;
  `dolt_writer_mode()` returns it. It is not a setting a session or role
  can change.
- **A protect flag per branch**, `dolt_branch('--protect' | '--unprotect',
  name)`, takes owning the database. It protects release or integration
  branches in any mode, or unprotects `main` in a branch mode for a
  migration. The `branch-*` modes protect `main` unless it was
  unprotected.
- **The writer** is the identity changes are attributed to: `dolt.writer`
  (`USERSET`), by default unique to the backend (`s<pid>.<backend start
  time>`), so a writer is a session unless set. `ALTER ROLE app SET
  dolt.writer = 'app'` makes an application's connections one writer.
  An id is at most 32 bytes of letters, digits and `.`, `_`, `-`, `@`,
  `+`, so a writer branch's name has room for its base. Changing it in a
  transaction that has written is refused.

**The invariant** (`owned`, `branch-only`, `branch-auto`): every change
in a branch's working set since its head was made by one writer, the
branch's owner. So `dolt_commit`, `dolt_status` and `dolt_diff('HEAD',
'WORKING')` describe one writer's work. A protected branch has no plain
writers: it changes only through integration operations that commit in
the same transaction, so its working set is its head at every snapshot.

**The owner** is recorded on `pg_branch`, in two nullable columns,
`browner` (the writer, `text`) and `brownerrole` (the role that claimed
it, `oid`), a catalog version change. A claim is an MVCC row update, not
a refs edit: the refs are held per database to transaction end
(`lock_refs()`), which would serialize every claiming transaction. The
owner is set at the claim (a writer's first write to a clean branch) and
cleared by what makes the working set its head: `dolt_commit`,
`dolt_reset('--hard')`, an integration commit, and the commit spec 15's
`dolt.transaction_commit` makes at `COMMIT`. "Dirty", for the modes, is
"has an owner". That costs O(1) and errs toward dirty (changes undone by
hand still leave an owner until a commit or a reset). The owner is
cleared only if no other backend of the same writer holds the ownership
lock (that backend's changes aren't in the commit's snapshot) and the
working set, read with a new snapshot, is still the head's; otherwise it
stays, which only makes another writer wait for the owner's next commit.
`brownerrole` has no `pg_shdepend` entry: dropping a role that owns a
dirty branch isn't refused, and the owner's role then shows as `?`.

**The ownership lock** is an object lock on the branch's `pg_branch`
row (`LockDatabaseObject()`, a sub-object of its own), on the default
lock method: transaction-scoped, so it moves to a prepared transaction
with the owner record. (The branch lock above is session-level and stops
deletion; it is a different lock.)

**Claiming**, at a transaction's first write to branch B by writer W:

1. B's owner is W: take B's ownership lock in `ShareLock`, then look
   again with a new snapshot. Still W, and proceed: W's backends write B
   concurrently, with row locks as on any table. No owner now (another
   backend of W committed meanwhile), and this is case 3, claiming alone
   in `ExclusiveLock`; another owner, case 2's error. Without the second
   look, a backend of W could write after the commit cleared the owner,
   and another writer could then claim a branch holding W's changes.
2. B has another owner: fail at once, `branch "B" has uncommitted
   changes by writer "X"`, with hints (`dolt_checkout('-b', …)`, ask X to
   commit, `dolt_branch('--take', …)`). Waiting couldn't change it.
3. B has no owner: take B's ownership lock in `ExclusiveLock`, which waits
   for in-flight writers' `ShareLock`s (bounded by `lock_timeout`, found
   by the deadlock detector; a `NOWAIT` form exists), then look again
   with a new snapshot: an owner now is case 2's error; none, and the
   owner becomes (W, the current role).

A claim needs the current role to be the owner's recorded role, unless
the role may take branches over (below), so a session can't set
`dolt.writer` to another's id to write into its branch.

**One check, `branch_note_write(branch)`**, shared with spec 15's
"changed" flag (one hook for both), is called from:

- the `prolly` table AM's write paths (`tuple_insert`, `multi_insert`,
  `tuple_update`, `tuple_delete`, and what rewrites or truncates a
  table), so the AM becomes a copy of heap's routine with them wrapped;
- the helpers that stamp a branch on a catalog row
  (`catalog_row_with_branch()`, `catalog_slot_set_branch()`,
  `branch.c:407-440`), for DDL of every kind;
- `nextval()` on a versioned sequence;
- the `dolt_*` calls that change a working set (`dolt_commit`,
  `dolt_reset`, `dolt_merge`, `dolt_cherry_pick`, `dolt_revert`,
  `dolt_conflicts_resolve`), explicitly.

Not from the lock path: `LOCK TABLE`, `VACUUM` and `ANALYZE` don't claim.
Nor from the branch operations that write on a session's behalf
(`materialize()`, branch creation, checkout, `sweep_detached()`): they
build a branch's state at its head, which leaves it clean.

**Protected branches and integration.**
- `dolt_merge`, `dolt_cherry_pick`, `dolt_revert` and `dolt_reset` into a
  protected branch are allowed when they commit in the same transaction,
  under the branch's ownership lock in `ExclusiveLock` (the merge already
  commits in its transaction, `diffmerge_folded.c:2967-2982`). A merge
  that would stop with conflicts is refused, not left in progress.
- A push, a fetch or `dolt_branch` that moves a protected branch's head
  is allowed for the database's owner, as an integration operation.
- `dolt_integrate([target], '-m', msg)`, from a writer branch, in one
  transaction: commit the writer branch if dirty; lock the target in
  `ExclusiveLock`; merge into it (fast-forward or a merge commit) and
  commit; fast-forward the writer branch to the target's new head. On a
  conflict it fails and everything rolls back, the first commit too; the
  error names the tables and suggests merging the target into the writer
  branch to resolve them there. It writes two branches, but commits each
  itself, so under `dolt.transaction_commit` neither counts as changed
  (spec 15, part 3) and its transaction makes no further commit.

**Writer branches** (`branch-auto`). A session on protected branch P
moves to its writer branch for P at the start of its first writing
statement (`ExecutorStart` for a plan that writes, `ProcessUtility` for
DDL), before the statement runs. A write hidden in a reading statement
(`SELECT nextval(…)`) errors, with a hint to make the write its own
statement or check out the writer branch first.
- **Forked from the snapshot's head.** P's working set is its head at
  every snapshot, so the transaction's earlier reads saw the head its
  snapshot sees; the writer branch is made there, not at P's current
  head. If P moved since the snapshot (`head_moved_unseen()`,
  `refs_copies.c:406-445`): a serialization failure under repeatable
  read and serializable, a new snapshot under read committed; under read
  committed a new one too if only P's baseline moved (`see_head()`).
- **Made head-only**, O(1) (`copies_branch_create()`,
  `refs_copies.c:1003-1048`); its copies are made at that first write
  (`materialize()`), so read-only sessions never pay for them.
- **Rollback.** An abort, or a rollback to a savepoint before the switch,
  undoes it: `undo_new_branches()` (`branch.c:617-636`) puts the session
  back when the branch was made in the aborted transaction, and is
  extended to a writer branch that already existed.
- **Named** `writer/<writer>/<base>`, reserved as `detached/` is
  (`reserved_branch_name()`, `prolly_dolt.c:392`). A name that wouldn't
  fit a branch name (`NAMEDATALEN`) is refused at the switch, with an
  error naming it, rather than cut. The writer and base, and every
  branch's protect flag, are kept in the refs, not in `pg_branch`, so a
  head-only branch has them, they travel with a push and a clone, and the
  name is never parsed.
- **Privileges.** Making a writer branch doesn't take `CREATE` on the
  database; reusing one (a later session with the same `dolt.writer`)
  takes the role that made it.
- **Reuse and cleanup.** A writer branch persists and goes stale as P
  advances; `dolt_integrate` fast-forwards it, and reusing it doesn't
  refresh it (`dolt_merge('<P>')` does). Writer branches that are clean,
  merged into P and not checked out are swept as detached ones are
  (`sweep_detached()`, `prolly_dolt.c:479`); there is no sweep at a
  session's exit. Remote listings leave them out, as they leave out
  `detached/`.
- `dolt_branch('-c', …)` copies the protect flag, never the owner.

**Visibility and administration.** `dolt_branches` gains `kind`
(`branch`, `writer`, `detached`), `owner`, `owner_role`, `protected` and
`base`. `dolt_branch('--take', name)` takes a dirty branch's changes as
one's own; it takes owning the database or the owner's recorded role, as
does `dolt_reset('--hard')` of another writer's changes. A standby shows
owners and stays read-only; `branch-auto` never forks there.

**With other decisions.**
- Step 5's "reset, merge and checkout of a commit on a branch other
  sessions use change its working set under them" holds in `shared`. In
  the other modes those operations need the branch's ownership (or a
  clean branch and a claim), and on a protected branch they are
  integration operations.
- Dirty merges (step 5's notes): in the other modes, the uncommitted
  changes are the merging writer's own.
- With `dolt.transaction_commit` (spec 15) the modes combine as they are,
  documented there (part 7): when the writer is one backend (the default,
  a session) phase 1 commits exactly the transaction's changes; a writer
  of many backends (`ALTER ROLE … SET dolt.writer`) shares its working
  set among them, as `shared` does among sessions, and phase 2 is what
  narrows each commit to its transaction. The commit the mode makes
  clears the owner, as `dolt_commit` does.

**Phases**, each a stack of PRs with its tests: `owned` (`dolt.writer`,
the owner record, the ownership lock, `branch_note_write()`, the
`dolt_branches` columns, `--take`); then `branch-only` (the mode,
protected branches, `dolt_integrate`); then `branch-auto` (writer
branches, the switch and its rollback, reuse, the sweep). Still open
(`08-open-questions.md`): whether the default writer is the session or
the role, revisited with data from `owned`'s use.

**As built** (#251, #256, #262, #265, not merged), where it departs from
or settles the above:
- `dolt_writer_mode([mode])` reads and sets the mode, kept as a setting
  in the refs.
- `dolt_branches` gains `owner`, `owner_role`, `protected`, `kind` and
  `base`.
- A branch's protection is stored only where it differs from what the
  mode gives it (`main` protected in the `branch-*` modes); a setting
  equal to the mode's default is removed, so a change of mode moves
  every branch left at its default.
- Writer branches' records (writer, base, the role that made them) are
  settings in the refs, not part of a head's entry.
- No sweep at a session's exit, and no refresh of a stale writer branch
  when it is reused (both above).
- `brownerrole` has no `pg_shdepend` entry; a dropped role shows as `?`
  (above).

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
- **The copy's locks (#489, as built).** The copy (made at a branch's
  first use) holds `ShareLock` on every versioned relation of the source
  branch, so no writer is in the middle of a page it shares. It doesn't
  queue for them: it tries them all (`ConditionalLockRelationOid`), and
  if one is held by a writer, gives back those it took and tries again,
  1 ms later, doubling to 100 ms (wait event `BranchCopyLock`). A writer
  that comes meanwhile goes ahead, never behind the first use. With
  `lock_timeout` set, the first use gives up once it passes, with a hint
  to try again or raise it; at the default 0 it waits as long as it takes
  (the deadlock detector doesn't see this wait; `statement_timeout`
  bounds it too). Under the locks only the map copy, the reference
  counts, the epoch raise, the marking of cached pages shared and the
  write of what was dirtied since are done: the source's dirty buffers
  are written beforehand in one pass over all its relations, and the
  page store is synced once per store at commit, after the locks are let
  go (a page the source changes after them is copied first, its old
  contents written to the shared block, so the blocks hold what the copy
  starts from either way; it only has to be durable when the copy is).
  Sharing the source's last fold instead, with no lock writers wait for,
  is the issue's second part, not built.
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
- **Freezing before pages are shared (#488, as built).** A VACUUM of a
  relation whose storage shares pages (`smgrcanshare()`: block-mapped
  storage) freezes with a freeze age of 0 when
  its caller gives none, so a page is frozen and marked all-frozen while
  it is still the relation's own, in place; a branch's copy then shares
  it all-frozen (its visibility map goes with it), and an aggressive
  VACUUM of either branch skips it rather than copying it to freeze it.
  An access-method default applied in `vacuum_get_cutoffs()`, not a
  reloption written at `CREATE` (which would enter the table's canonical
  description and its commits' hashes): `vacuum_freeze_min_age` set
  explicitly (in the configuration file, for a database or role, or in
  the session) or the relation's `autovacuum_freeze_min_age` reloption
  still decides. A normal VACUUM then also scans every all-visible page
  not yet all-frozen (PG18's eager scan, unbounded: no minimum table
  size, no cap on successes, no failure limit per region), since reads
  set pages all-visible (on-access pruning) without freezing them and a
  normal VACUUM would skip those; an explicit
  `vacuum_max_eager_freeze_failure_rate` (setting or reloption) leaves
  the eager scan as PG18 has it. The cost: freeze WAL earlier, and
  reading all-visible pages until they are frozen.

## 7. Step 6 design: remotes

**Status: decided by the user, 2026-10-17, as recommended** (7.9 lists
the decisions); file remotes built (7.11), network remotes not yet.

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
  `dolt_remotes` view (`name`, `url`, and Dolt's `fetch_specs` and
  `params`, `jsonb`: the one fetch spec, `refs/heads/*:refs/remotes/<name>/*`,
  and `{}`, a remote being its URL (#586)); kept in the refs tree (a new
  tag), not in a catalog, so they move with the refs' transactions.
- **Remote-tracking branches** (`origin/main`): heads only, written by a
  fetch, never with copies; `dolt_checkout('origin/main')` is refused with
  a hint to make a branch from it, and `dolt_checkout('-b', 'x',
  'origin/x')` makes one at its head (head-only, as any new branch).
- **Upstreams**: a branch may name one (`dolt_push('--set-upstream', …)`,
  `dolt_branch('--set-upstream-to', …)`), for `dolt_pull()` and
  `dolt_push()` without arguments, and for `dolt_branch('-d', …)`, which
  refuses a branch not merged into its upstream's remote-tracking branch
  (as of the last fetch), or into the session's branch when there is none
  there, as Dolt's `validateBranchMerged` (and git) does; the session's
  own branch and `main` are refused first, as in Dolt.  A deleted
  branch's upstream stays, as Dolt keeps its branch config: a branch made
  again of its name has it.

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

- **Shared dependencies** (part 1). **Decided by the user, 2026-10-17:**
  each commit records what its versioned objects need from outside
  them, and a clone installs the extensions and checks the rest before
  anything is written. **Built** (#188, #189, #192): as
  below.
  - **Recorded at each commit:** an entry per dependency outside the
    versioned schemas, by kind and qualified name: an extension with its
    version and schema (the versioned objects name its members by
    schema), a type, function, operator, collation or other object of an
    ordinary schema (with its address, which another database resolves by
    name with `pg_get_object_address`); each value a `text[]` literal, so
    any name reads back whole. Not an extension's members as dependers;
    an extension a versioned object names with `DEPENDS ON EXTENSION`,
    yes. Described once per commit, however many objects need it, and
    only at a commit, never at a fold. Which objects each versioned object depends on is found through
    `pg_depend`; every commit describes each dependency afresh (an
    extension's version, an object's name, by OID), so an
    `ALTER EXTENSION … UPDATE` or a rename outside the versioned schemas
    shows in the next commit, though no versioned object changed.
  - **In the commit, not the root (user, 2026-10-17).** The entries are a
    small tree of their own, which the commit names in a "dependencies"
    field beside its root, as it names its parents (not text in its
    message). The root stays what the versioned objects are, so what
    changes outside never makes a branch's working set differ from its
    head: no dirty branch after an extension's update or a checkout, no
    rows in `dolt_status`, no uncommitted change blocking a merge.
    Transfer and reclamation reach the tree through the commit, as they
    reach its root; the commit id covers it. A commit made before the
    field existed has its dependencies unrecorded (known, not "none"). No
    new canonical format: roots are unchanged.
  - **Not roles (user, 2026-10-17).** Owners and grants aren't part of
    the canonical form: a commit doesn't record them, and a move or clone
    doesn't restore them (objects are made by whoever runs it). A role
    recorded as a dependency would make a branch read as changed after
    its own checkout, or a clone by another user, though nothing changed.
    As Postgres: roles are the cluster's, not a database's (`pg_authid`
    is shared), and `pg_dump` of a database never writes `CREATE ROLE`
    (roles come from `pg_dumpall --roles-only`). Dolt and Doltgres agree:
    users and grants are in the server's `privileges.db` and `auth.db`,
    shared by every branch and never in a commit. So no role is recorded
    at commit or checked at clone. Roles would become dependencies only
    if owners and grants (columns of a branch's copies' catalog rows)
    were ever versioned, as `pg_dump` writes them.
  - **At clone, checked before anything is created:** the dependencies of
    every branch's head, not only the default one's, against the
    database: each extension against `pg_available_extension_versions`
    and `pg_extension`, each object by its address. A clone missing any
    refuses once, naming all of them: an object not in the database (an
    object is looked up by its address alone); an extension not installed
    on this server, not at this version, in another schema than recorded,
    or recorded in schemas that differ across branches. Only then, before
    anything else is created, are extensions created, in their recorded
    schemas, each after the recorded ones it requires; `CASCADE` makes
    what no commit recorded, at its default version and in the requiring
    extension's schema. An extension recorded
    without a version needs any. A head that didn't record its
    dependencies isn't checked. Only a "not found" error means an object
    is missing: anything else (a cancel) is raised.
  - **An extension's version, across branches:** one version serves a
    database, and branches may have recorded different ones (one branch's
    head made before an update, another's after). A clone installs the
    newest version any branch recorded, newest by the extension's update
    paths (`pg_extension_update_paths`): a move to a branch that recorded
    an older one needs no change (an update keeps what older versions
    made). Versions no update path orders refuse the clone, naming them.
  - **An extension the database has already** (an empty database for a
    clone has no versioned schemas, commits, other branches, remotes or
    upstreams, but may have ordinary schemas, roles and extensions, the
    dependencies themselves): at the version the clone needs, or a newer
    one, it is used as it is; at an older one, the clone refuses, with a
    hint to update it. A clone creates only extensions the database
    lacks, and never alters one there.
  - **At a move** (checkout, merge, reset, cherry-pick, pull): an
    extension is every branch's, so a move never creates or alters one. A
    move to a commit needing an extension the database lacks, or a newer
    version than it has, refuses with a hint to install or update it
    first (`ALTER EXTENSION … UPDATE`, which the user runs, for every
    branch at once). Other missing dependencies refuse as at clone. As
    built, each checks what the move brings in, not all its target
    records: a branch's first use (by `dolt_checkout` or a connection to
    it) its head, checked before the refs are held; a reset what its
    target needs that the head doesn't (a reset to the head, or an abort,
    nothing); a merge (a pull's too) what the incoming head needs that the
    merge base didn't; a cherry-pick what each commit needs that its
    parent didn't, and a revert what the parent needed that the commit
    doesn't. A checkout of a branch that has its copies moves nothing.
    Measured by recorded entries, a new use of something the base already
    needed isn't a new need; the copies failing still catches what the
    check can't see.
  - **Not as decided: not in diffs.** The decision (above) had
    `dolt_diff` and `dolt_status` show dependencies changing. As built
    they compare roots, and the dependencies are beside the root, so
    neither shows them; a commit's are read with its other fields. Not
    built yet: a diff of two commits' dependency trees, for those
    readers.
  - **The backstop:** `pg_depend` doesn't see what a function's body
    names (its source is text), so the list can't be complete; making the
    copies still fails, rolls back and names what it lacked, as before.
  - **Not versioned:** extensions themselves. One version of an extension
    serves a database, and the server loads one copy of its library for
    every database, so per-branch extensions couldn't isolate what an
    upgrade changes. An upgrade is tested in a clone (`CREATE DATABASE`,
    `dolt_clone`, `ALTER EXTENSION … UPDATE` there, the tests), then made
    in the database.
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
- Row security doesn't narrow a fetch: commits hold policies (since the
  objects stack), but history holds every row whatever they say, so
  fetching takes the roles above and bypasses them, as a BYPASSRLS role
  does (further than `pg_read_all_data`, whose reads policies still
  filter).
- **Writer modes** (6.2, decided 2026-10-18): changing a database's
  mode and a branch's protect flag take owning the database. A push, a
  fetch or `dolt_branch` that moves a protected branch's head is an
  integration operation, allowed for the database's owner. Taking over
  another writer's dirty branch takes owning the database or the owner's
  recorded role. Writer branches don't take `CREATE` on the database;
  reusing one takes the role that made it.

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
check: each chunk re-hashed before anything reads it, a chunk the remote
lacks an error (so every reference resolves, in what was received or in
the store), each received commit's root of this build's format, and only
then one table file added. The typed walks stop at a commit or root the
visitor declines (#171), so a fetch or push reads what changed, not the
history.

Not as designed, or not yet:

- **The walk isn't batched or overlapped.** It asks the remote for a chunk
  at a time, as the walks read them; over a file remote that is a read.
  Batching (7.1) matters over the network and comes with it (7.12: batched
  by node, not overlapped).
- **Clone walks** rather than copying the remote's files whole and then
  checking them (7.4): the same bound, O(B); the whole-file copy waits for
  V4 to say what it saves. (Since spec 22's item 20, a clone into a
  database with nothing to walk against copies them whole and resumes,
  unless the remote can't list its files or its journal is a fifth of it
  or more: those still walk.)
- **The store's own chunks are trusted.** A commit or chunk the store
  has is declined without reading what it references: the receive check
  resolves references through the store's closure, which it assumes, as
  7.1 does. A store whose closure broke (a GC that removed a chunk a
  transfer was told it had) would be admitted unchecked: step 7 must keep
  the closure (8.2, 8.3), and the receive doesn't verify it. As built
  (8.11), the receive still doesn't: what it adds goes to a table file
  unchecked. What keeps the closure is the rest: the append check on
  what transactions put (the references the tree format declares; other
  hashes only against what a swap dropped), a full collection keeping
  what the files it keeps whole name, and a transfer that began before a
  collection's swap refused.
- **The seen sets aren't bounded.** What a receive has seen (each chunk
  received, its hash in memory, its bytes in a temporary file) and what a
  send has seen are hash tables in memory, growing with the chunks moved;
  7.1 bounds them by `maintenance_work_mem`, spilling past it. (Since
  spec 22's item 20, bounded so.)
- **Shared dependencies.** Built since (7.4): a clone checks every
  branch head's recorded dependencies before it makes anything, and makes
  the extensions the database lacks; a move to a commit refuses what the
  database lacks. A head made before commits recorded them isn't checked:
  only the default branch's copies are made at clone, so a clone whose
  default branch's head lacks something fails where its copies are made,
  naming it, and leaves nothing, and one whose other branch's does
  succeeds, that branch failing at its first checkout.
- **A push moves the remote's head as it is reached,** not with the
  transaction: the remote-tracking branch moves with the transaction, so a
  push whose transaction aborts leaves it behind the remote until the
  next fetch.
- **Not built:** network remotes over libpq, the transfer from known
  heads, `pg_prolly_fetch` and `pg_prolly_push` (they guard the remote
  functions, which come with them), deleting a remote's branch (so
  `dolt_fetch --prune` too), resuming, and V4. (All but deleting and
  resuming since: 7.12.)

### 7.12 As built (network remotes)

Built: decision (A) of 7.5, behind the remote interface (#198–#203).

- **Serving.** A database serves as a remote through `dolt_serve_*`
  functions: its format and heads (each branch with a commit), `has` and
  `get` of a batch of hashes, what changed since known heads (below), and
  for a push `receive` (batches of chunks kept in a temporary file for the
  remote's transaction) and `move_head`. The move answers false at once
  if the branch isn't at the head expected; otherwise it receives what was
  sent as a fetch receives (the transfer's receive, through the remote
  interface over that file: each chunk re-hashed, every reference
  resolved, the commit of this build's format) before anything is added,
  then moves the branch with the refs' new `branch_move()`: a
  compare-and-swap of its head; a branch it hasn't made at the commit,
  with only its head; one with copies read as of its head (a snapshot
  older than it refuses, or is renewed), refused over uncommitted changes
  unless `--force` (which drops them), as 7.3 decided, and over what the
  commit needs that the branch's head doesn't (7.4), as any move is; then
  moved there, its rows written, a merge in progress there ended, as a
  reset ends one. A branch without commits whose working set holds
  anything (an empty versioned schema included) counts as changed. A null
  head deletes the branch (refused over uncommitted changes unless
  forced, one already gone deleted as asked): a destructive entry point,
  granted as pushing is, though no client asks for it yet.
- **Privileges (7.6).** The predefined roles `pg_prolly_fetch` and
  `pg_prolly_push`. The fetching functions' EXECUTE is granted to both
  (a push reads heads too), the pushing ones' (`receive`, `move_head`) to
  `pg_prolly_push` only; each checks the database's owner or, fetching,
  either role, pushing, `pg_prolly_push`. So an owner who isn't a
  superuser needs one of the roles, or EXECUTE granted, to be fetched
  from: a revoked EXECUTE can't be given to "the owner of the current
  database".
- **The client.** URLs `postgresql://` and `postgres://` (libpq's), served
  by a loadable module, `prolly_remote_libpq`, as replication's
  `libpqwalreceiver` is: core doesn't link libpq. A role that isn't a
  superuser must give a password in the URL and the connection must use
  it, as dblink asks. A remote's errors come back with their SQLSTATE.
  A remote opened is a session there in transactions: a push's
  `move_head` commits its own (rolls it back if refused), and the next
  statement begins the next; a push of several branches isn't atomic
  across them. TLS is the connection's (`sslmode`), tested against a
  server that takes TLS only.
- **Batching (7.1), by node.** A tree walk tells the chunk store what it
  reads next (a node's children, a leaf's value trees: `prefetch()`, only
  when a hint would hear it), and a store reading through a source passes
  it on (`read_through()`'s hint): a receive asks the remote at once for
  those it lacks (`get_many()`, keeping only what it asked), a send at
  once which of them it has (`has_many()`). A batch is a node's fan-out,
  not 7.1's 64K breadth-first frontier, and checks aren't overlapped with
  fetches. `t/048` checks a push's has-checks take few requests; a fetch
  from a remote without the transfer below (a file remote, an older
  build) is batched so.
- **Commit heights (user, 2026-10-20, #362).** A commit records its
  height, 1 + the most of its parents' (a root's 1), as Dolt's do: a
  "height" item beside "dependencies", so the commit id covers it. The
  walk below goes highest first, and a recorded height means a commit's
  history isn't read to find it: a call reads the new commits and its
  frontier, not O(history). A commit without one (made before heights
  were recorded, or by a build without them) has its height found from
  its parents', depth first, each commit read once; a commit made on such
  history records the height found, so the walk is made once.
  `pg_stat_prolly`'s `commits_read` counts commits read.
- **From known heads (7.1).** The receiver names every head it has
  (branches and remote-tracking heads), and a remote that runs this code
  (`get_since()`, `dolt_serve_since()`) sends, in one result read a row at
  a time (built whole on the server first, spilling past `work_mem`), the
  commits from the wanted down to the known heads or their merge bases
  with the wanted (what the receiver has below its heads), each with its
  own chunks, its dependencies tree whole, and what its root changed from
  its first parent's (the tree's `diff_nodes()`, the canonicalizer's
  `walk_changes()`: equal subtrees skipped). With no known heads (a clone)
  it sends the history: O(B), as the walk is. What it sends is kept as
  anything received is, taken into the store only where the walk from the
  wanted commits reaches it (and not where the store has it already),
  and the walk asks for what wasn't sent. `t/048` checks a pull of 30
  commits in a line takes one such exchange and no `get`, an emptied
  table nothing more, and a fork behind the head little; a remote of an
  older build, without the function, serves by the walk. A file remote
  tells nothing (it runs no code).
- **V4** (`06-benchmarking.md`): a second local instance clones over
  `postgresql://`, fetches a commit the origin made since, and pushes one
  back to a new branch; each timed, and checked to have moved what it
  was timed moving. Measured in step 8.
- **Still not built:** a client deleting a remote's branch (so
  `dolt_fetch --prune`), resuming a transfer, a bounded seen set, the
  whole-file clone. (Since spec 22's group E: a bounded seen set, the
  whole-file clone, and resuming a clone; a fetch or push still starts
  over.)

## 8. Step 7 design: reclamation

**Status: decided by the user, 2026-10-17, as recommended** (8.9 lists
the decisions); built and merged in #210–#225, as 8.11 says.

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

When spec 14's C is built, the working set becomes a root too: TOAST
pointers that name chunk-store content live in versioned heaps, so a
collection scans those heaps, dead tuples included, for the hashes they
hold, and keeps chunks appended after the oldest replication slot's
position, and the value trees hash pointers in the WAL from there name,
which logical decoding may still read, a standby's slots' too (spec 14,
C4 and #300). The heaps' scan reads only the page-store blocks written
since the last collection (#340).

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
   INDEX CONCURRENTLY` does, but not for a prepared transaction, which
   reads nothing more), so no backend reads a file it opened
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
  after a crash. (As built, #564: the chunks a collection copies are
  logged as where they come from, a `COPY` record redo copies again from
  its own files; their invariants are in spec 22, part 6, G.) A crash before the swap leaves the new files unnamed: at
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

### 8.11 As built

Built in a stack of PRs, in 8.10's order: the append check (#210), journal
segments and the manifest's settled files (#211), the collection and
`dolt_gc()` (#212), the full collection (#213), the page sweep (#214),
automatic collection and sweeps under autovacuum (#216), and the
measurements (#223).

- **The append check** (8.2). Each batch a transaction appends, and the
  rest at pre-commit, is checked against the store as of a GC generation,
  and appended only if no swap came meanwhile (only a swap removes
  chunks); so is the root a transaction moves to, before its PAD (it may
  be an older root the transaction didn't write). A subtransaction that
  aborts takes the chunks it put with it. What a chunk names
  is the tree's to say: a new tree callback, `references()`, visits a
  node's children and out-of-line value trees from its bytes alone.
- **Segments** (8.3's seal). The manifest (version 2; version 1 is read)
  names the journal's segments, oldest first (`prolly_journal`, then
  `prolly_journal_<id>`), whether each table file is settled, and the
  files retiring. A PAD names its transaction, and its ROOT may be written
  into a segment sealed before its commit.
- **The collection** (8.3). Behind the reclamation interface: a collector
  ("copying") over new chunk-store primitives (`gc_begin`, `gc_files`,
  `gc_find`, `gc_read`, `gc_new`, `gc_copy`, `gc_swap`, `gc_retire`).
  Seal, mark and copy, swap and retire as designed, with SEAL, SWAP and
  RETIRE manifest records; copied chunks are re-hashed as they are
  copied, before the window, and those marked in the window in it (so the
  window's time includes their reading, hashing and writing);
  the retire waits for the swap's virtual transactions, and a standby
  cancels the queries that were running when it replayed the swap
  (`ResolveRecoveryConflictWithFileReaders()`, a thin export from
  `standby.c`). A transfer that began before a swap is refused (8.3's
  generation check). One collection at a time, a second refused rather
  than left to wait (the first waits for every transaction running at its
  swap, the second among them); none in a transaction that used the
  store; one begun in a subtransaction that aborts is abandoned with it.
  `dolt_gc()` and `dolt_gc('--full')`: the database's owner or
  `pg_maintain`.
- **Generations** (8.4). A default collection doesn't collect settled
  files, so its marking ends there; a full one collects every file and
  keeps whole each settled file at most half garbage, marking whatever a
  kept file's chunks name, live or not, so the store stays closed under
  what its chunks name (7.11's trust holds); chosen again until none is
  newly kept, as what kept files name counts as live where it is. Nothing
  a kept file holds is on the swap's dropped list.
- **The page sweep** (8.6). `bmap_sweep()` per tablespace and database,
  `prolly_page_sweep()` for the current database: exact counts of what
  every map and changes list holds, read under each map header's lock;
  blocks nothing holds freed, stuck counts rewritten once they fit, the
  store shortened (a SHRINK record). Branch creation waits for it while it
  counts; while a page store is swept its allocator reuses no freed block
  (other stores' do). Its
  crash tests found a bug: a write evicted while its backend held the map
  page holding the evicted page's entry locked it again. #214 worked
  around it (the write copied a page its backend held locked); #227 fixed
  it at its cause and removed the workaround: a change locks its map page
  only after every buffer it reads, which assert-enabled builds check at
  every read (`t/050`, `t/056`).
- **Automatic** (8.5). After its tables, an autovacuum worker collects
  the chunk store when what a default collection collects (the journal
  segment, and the table files not settled) has passed
  `prolly.gc_journal_threshold` (128MB; -1 is off) and exceeds the
  settled files' size times `prolly.gc_growth_scale_factor` (0.2), read
  from sizes alone, its reads paced by the cost-based delay; and sweeps a
  page store when one is due (below). Both are SIGHUP settings. A worker
  started only to prevent wraparound, with autovacuum off, does neither.
- **Tests.** Regression: `append_check`, `journal_segments`, `gc`,
  `gc_full`, `page_sweep`. TAP: `054_journal_segments` (a ROOT in a
  sealed segment, a failed seal, standby, crash), `055_gc` (writers
  straddling the swap refused or committing, a root moved to a dropped
  chunk refused, a second collection refused, one failed in a
  subtransaction, a transfer refused, a kept file's garbage named across
  a full collection's swap, standby files and a cancelled standby query,
  a crash at each step), `049_page_sweep` (crashed prune and drop leaks
  freed, a share waiting, another store reusing blocks meanwhile,
  standby), `050_bmap_evictions` (with 16 buffers),
  `051_autovacuum_reclaim`, `052_gc_measure`.

**Measured** (`052_gc_measure.pl` at `PROLLY_GC_MEASURE_SCALE=50`,
`PROLLY_GC_MEASURE_BRANCHES=50`: a million rows, 50 branches each
changing 50,000 of them; an assertion-enabled debug build on the
development machine, so indicative, not step 8's numbers; prolly_bench's
new V6 workload measures the space and the collections' times on an
optimized build, not the window under committing sessions):

| Measure | Result |
|---|---|
| Chunk store: base, with the branches | 50.9 MB, 102.7 MB |
| After a default collection; the branches deleted and collected again | 102.1 MB; 102.1 MB (their chunks settled) |
| After a full collection | 50.9 MB |
| Default collection: the base; the branches' changes; nothing new | 255 ms; 248 ms; 12 ms |
| Full collection after the deletes; again, all live | 377 ms; 276 ms |
| The window, under four sessions committing | median 0.14 ms, max 0.16 ms (the collection: median 18 ms); the upper of the two middle samples of ten (the test now reports their mean) |

A default collection costs what changed (the branches' changes cost
about what the base did, nothing new costs almost nothing). The window
reads, hashes and writes what was appended during the collection, not
the store; it also passes over every collected slot's mark (bits, nothing
read) to find those, and over the manifest's files, which grows with the
store but was too small to see here.

Not as designed, or not yet:

- **Marking is conservative over bytes**, not the refs' typed walk:
  wherever a chunk's bytes hold a hash's length and then a collected
  chunk's hash, that chunk is marked. Every reference the store's chunks
  hold is written that way, so nothing named is lost, whatever the kind of
  chunk; bytes that happen to spell a collected hash keep a stray chunk.
  (Since spec 22's item 17, marks follow what each chunk declares.)
- **The append check covers what the tree format declares.** Hashes a
  chunk holds otherwise (a commit's parents and root, the refs' heads, a
  canonical root's data trees) are checked, for a transaction that began
  before a swap, against a list of what the swap dropped (the chunks not
  yet marked before the window, written beside the files, removed at the
  retire): a stray hit fails a straddling transaction needlessly, never
  the other way. A transaction that began after a swap but uses a hash a
  backend kept from before it isn't checked so.
- **Writers with batches in the sealed segments are waited for** before
  anything new is read, rather than their chunks kept as roots (copied to
  a settled file, an uncommitted chunk would be everyone's before what it
  names is). A long transaction that appended a batch before the seal
  holds the collection there. A prepared one, which may stay for days,
  isn't waited for (here, for a transaction that named a tree before the
  collection began, or at the retire): its chunks in the sealed segments
  are roots instead (what they name was appended before them, so it is
  copied with them; it can't have moved the root, so no PAD of its
  becomes a ROOT), and its pointers are on pages already. A transaction
  prepared while it is waited for is waited for until then: the wait is
  for its backend's virtual transaction id
  (`VirtualXactLockUnprepared()`, beside `VirtualXactLock()` in
  `lock.c`), not for its xid, which the prepared transaction keeps.
- **The manifest's lock is let go at the swap**, and a lock of the
  collection's own held to the end of its transaction (past the retire):
  a transfer waiting for the manifest would otherwise deadlock with the
  retire's wait.
- **The window holds `ProllyJournal`**, which is the server's, not the
  database's: every database's appends wait for it. (Since spec 22's
  item 14, the database's own append lock; the server's lock is gone.)
- **Files a crash leaves** (a collection's unnamed new files, segments
  retiring) are removed by the next writer or collection, not at the end
  of recovery. The collection isn't resumed. (Since spec 22's item 16,
  removed as crash recovery ends; not after archive recovery or a
  promotion, whose readers may still read what is retiring.)
- **The standby conflict** covers every query running in the database
  when the swap was replayed.
- **The mark's frontier** is an array in memory, not bounded by
  `maintenance_work_mem`; the sealed segments' slots are a hash table of
  their chunks (what changed since the last collection). (Since spec
  22's item 15, the frontier spills past it.)
- **Counts past 255 aren't kept in a table of their own.** A block shared
  by more than 255 maps stays stuck while it is; the sweep rewrites its
  count once its holders fit, and frees it once none is left, so a block
  shared by a thousand branches is freed after they go, by a sweep rather
  than by the last drop.
- **The sweep's automatic trigger** isn't "allocated blocks exceed what
  the relations map by a fraction": with sharing, what the maps hold is
  known only by reading them, which is the sweep. A store is swept once
  after the server starts (a crash before may have leaked a drop's or a
  prune's blocks) and whenever a stuck count has lost a holder since its
  last sweep began (up to 1024 stores remembered). A relation dropped
  while a sweep reads it counts for that sweep: its blocks may wait for
  the next.
- **No automatic full collection.** The design's estimate of settled
  garbage, from the branches and commits dropped since the last full
  collection, needs those counted where the store keeps them;
  `dolt_gc('--full')` by hand meanwhile.
- **The rewrite threshold** (half) is a constant, not a setting, and the
  growth factor's default (0.2) is autovacuum's scale factor's.
- **Not built:** a reflog (8.1; since built, spec 22's item 4), resuming
  a collection, a collection that holds chunks a transfer was told the
  receiver has (8.3 refuses the transfer instead).
