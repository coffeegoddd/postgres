# 11. Pages for the working set, Dolt-style history

**Status: proposed (user direction, 2026-10-15); not built.** This
replaces the design in which prolly trees are a table's primary storage
(engines D1–D4, `04-candidate-designs.md`). The decisions below marked
"user" were made on 2026-10-15; everything else is a recommendation to be
settled before the step that needs it. Part 4 is the plan for getting from
the code as it is today, open PRs included, to this design.

## 1. The design

### Two layers

- **Prolly tables sit beside normal tables** (user). A database can hold
  both. A prolly table (table access method `prolly`) is versioned; a
  normal table isn't, and nothing here changes how it is stored or
  behaves.
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
  captures every prolly table in the database (user); normal tables are
  never in commits. A SQL commit
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

### Branches: each has its own copy of every prolly table (user)

- **Catalogs are ordinary, shared catalogs.** Normal tables, functions,
  types and everything else not tied to a prolly table look the same from
  every branch.
- **Each branch has its own copy of each prolly table** (user, option B):
  its own relation with its own OID, catalog rows and relation file
  number, and with it the table's indexes, TOAST table, constraints,
  triggers, rules, policies, extended statistics and owned sequences.
  DDL on a branch changes only that branch's copy, so branches can
  diverge in schema and merge, as in Dolt.
- **Copies share pages copy-on-write.** Under each copy's relation files is
  a **block map** from logical block to physical block in a page store
  shared by the database's prolly relations. Creating a branch copies the
  prolly tables' catalog rows and their block maps by reference: no page
  is copied. The first logical change to a shared page on either branch
  gives that branch its own copy of the page.
- **Names resolve to the session's branch.** Connections choose a branch
  by database name (`db/branch`, as built). A prolly table's name, in its
  schema, resolves to the copy on the session's branch; everything stored
  by OID that refers to a prolly table belongs to that branch's copy.
- **Objects that depend on prolly tables:**
  - Views and materialized views over prolly tables, and rules on them,
    are copied per branch with the tables.
  - Foreign keys between prolly tables are per branch; a prolly table may
    reference a normal table (shared data); a normal table referencing a
    prolly table is refused (its rows would point into one branch only).
  - Functions resolve names when they run, so they see the session's
    branch; nothing to copy.
  - Creating or dropping a prolly table on a branch affects that branch
    only.
- **Shared catalogs** (`pg_database`, `pg_authid`, tablespaces) stay
  cluster-wide and unversioned, as decided for question 27.
- A branch that no connection uses and that has no uncommitted changes can
  drop its page sets; it is then only its head commit. Its pages are made
  again when it is next used: copy-on-write from a branch whose head is
  close to it, then the difference applied (part 2), or, when no branch
  is close, materialized from the tree.
- Temporary and unlogged prolly tables are refused, as before (question
  28): a prolly table is permanent.
- **How copies are named (step 2's first decision).** Recommended: a
  copy keeps the table's schema and name, with a branch column added to
  `pg_class` and `pg_type` and to their name keys, and name lookups match
  the session's branch or no branch. Error messages, `regclass` output,
  `\d` and `pg_dump` then print the names users wrote. The alternative,
  a hidden schema per branch, leaves the catalogs' keys alone but leaks
  the hidden names wherever a schema is printed.

### What is in pages and what is in the chunk store

| | Pages (working set) | Chunk store (history) |
|---|---|---|
| Format | Postgres's: heap tuples, btree | Ours: prolly trees of canonical rows, canonical schema, root values, commits |
| Mutability | Mutable, MVCC | Immutable, content-addressed |
| Ordering | Postgres collations, opclasses | The canonical key encoding (memcmp order from btree opclasses) |
| Durability | WAL, checkpoints, full-page writes | WAL-logged appends to the store's files (below) |
| Per branch | Each prolly table's copy: catalog rows and block maps (copy-on-write) | Refs (branch heads, working-set roots) |
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
- **Versioned schema** comes from per-branch copies of prolly tables'
  catalog rows instead of catalogs on prolly trees:
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
| Create a branch at another branch's head | Copy each prolly table's catalog rows (new OIDs and relation file numbers) and its block maps by reference | O(prolly relations); no page is read |
| Create a branch at a commit | As above from a branch whose head is near it, then apply the tree diff between the two heads as row changes | O(Δ log N) |
| Dolt commit | Fold: find the branch's prolly rows changed since the head (below), write them into the head's trees, then the schema, root value, commit, ancestry and refs | O(changed pages + Δ log N + log H) |
| Working-set checkpoint | The same fold, published as the branch's working-set root instead of a commit | As a commit |
| Diff, log, status | Over trees: subtree-skipping diff; working-set diffs fold first | Dolt's bounds |
| Fast-forward, reset, checkout of a commit | Apply the tree diff from the current head to the target as row changes | O(Δ log N) |
| Three-way merge | Merge trees as Dolt does (base, the target branch's folded working set, the source), then apply the diff from the target's working set to the result as row changes; conflicts as `dolt_conflicts` rows, as built | Dolt's merge bound plus O(Δ log N) |
| Fetch, push, clone, pull | Our protocol, chunks by address; a cloned branch gets pages when first used | Dolt's bounds; first use O(N log N) materialization |

**Finding changes since the head.** The block map records, per branch,
the epoch of its last fold. A page first changed after that epoch is
copied (even when no other branch shares it), so its pre-fold version
survives until the next fold. The fold reads both versions of each such
page; every tuple on either is a candidate key; each candidate's new row is
read from the branch's pages through the primary key (or the content hash
for keyless tables), its old row from the head's tree, and equal pairs are
dropped. The cost is in changed pages, not in table size.

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

- **Merged on `master` (PRs #1–#11):** the interface stubs and registry,
  D1 tables and the prolly index AM, the copy-on-write tree, the
  cluster-wide node store, node counters and the subtree-skipping diff.
- **Open, stacked (PRs #12–#46):** the rest of step 4: snapshots, K1 row
  locks, spill, SERIALIZABLE, large values, GC, VACUUM, branches, diff and
  merge, canonical commits, the `dolt_*` SQL interface, conflicts,
  versioned databases, and the 16 fixes from Postgres's own suites (4h).

### What carries over

| Today | In the new design |
|---|---|
| `dolt_*` functions, views, `dolt.author`, `db/branch` connections, conflicts as in Dolt (#25, #26) | Kept; their backend changes to per-branch copies and the chunk store |
| `CREATE DATABASE … VERSIONED` and its refusal of heap tables (#27) | Superseded: version control is per table (prolly tables), so any database can hold both. Removed with D1 |
| Hash, key encoding, canonical rows, value trees, canonicalizer (4a, #18, #23) | Kept: the chunk store's contents |
| Copy-on-write trees, subtree-skipping diff, three-way merge, merge conflicts, refs, commits (4c, #9, #21, #22, #26) | Kept; they operate on chunks from the new store instead of node-store pages |
| Chunker, node format | Reworked: the byte-based rule (step 5's plan) and pageless nodes |
| Interface registry and stub pattern (`prolly_registry.c`) | Kept; the new interfaces register there |
| Requirements suite (`src/test/prolly`), `test_prolly`'s version-control tests, the benchmark harness, gates 1–7 | Kept; storage-engine tests go with D1 |
| The node store (`nodestore_global.c`), reclamation (#19) | Replaced by the journal and table-file store and its GC |
| D1 and K1 (table AM on trees, row locks, TID maps, keyings, spill, SERIALIZABLE at table granularity, the 4h fixes to them), the prolly index AM | Superseded by pages. Kept building until the new design is measured against them (step 8), then removed |
| `09-versioned-catalogs.md` part 4 stages 2–7, and #34 (catalog writes through the table AM) | Superseded by per-branch copies of prolly tables' catalog rows; #34 stays harmless |

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

**Step 2. Branches: per-branch copies of prolly tables.**
- The naming decision (part 1); copying a prolly table's catalog rows and
  dependent objects per branch; name resolution by the session's branch;
  the refusals in part 1.
- Branch creation copies block maps by reference and flushes the source's
  dirty buffers first; remapping shared pages (part 5); freeing a copy's
  pages past the xmin horizon; VACUUM and freezing on shared pages
  (visibility-neutral changes in place; line pointers made unused only
  when no other branch shares the page).
- `dolt_branch` and `dolt_checkout` on copies, no commits yet.
- **Exit:** DDL and TRUNCATE on one branch leave the others alone
  (`vc_schema` passes); normal tables are unaffected by branches; branch
  creation reads no table's pages; the S suite on a branch matches heap.

**Step 3. The chunk store, WAL-logged.**
- The journal and its index, table files, the manifest and the refs map,
  in the directory of each database with prolly tables;
  concurrent appends from backends serialized by a lock.
- The resource manager: chunk appends, the root in the commit record,
  table files, manifests, unlinks; redo and base-backup consistency.
- Pageless nodes and the byte-based chunker; the tree, diff and merge
  code reading and writing through the new store.
- **Exit:** the tree tests of `test_prolly` pass on the new store; crash
  tests at every record boundary recover to the last committed root; a
  standby's store matches the primary's byte for byte.

**Step 4. Dolt commits from pages.**
- The fold over the branch's prolly tables: the change finder, tree
  edits, the canonicalizer, commit and ancestry, refs and working-set roots; working-set checkpoints.
- `dolt_commit`, `dolt_log`, `dolt_status`, `dolt_diff` and the views on
  the new store.
- **Exit:** gate 6 (history independence) and the requirements suite's
  commit and history tests pass; commit cost tracks changed pages.

**Step 5. Moving heads.**
- The row applier (from logical replication's apply worker); checkout and
  reset to a commit, fast-forward, branch at a commit, materializing a
  branch with no pages.
- Three-way merge over trees into a working set with changes; conflicts
  as built; cherry-pick, revert.
- **Exit:** `vc_branches`, `vc_diff_merge`, `vc_history` and the
  conflicts tests pass; V1–V3 run, V3 with background writes.

**Step 6. Remotes.**
- File remotes first, then a network protocol of our own: fetch, push,
  clone, pull; a cloned branch materialized on first use.
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

### Selection

- smgr already dispatches through `smgrsw[]`; `smgropen()` gains a
  selector instead of always choosing md.c. A relation is block-mapped
  when it is a prolly relation: a prolly table, or an index or TOAST
  table of one. Temporary relations (a backend in the locator), shared
  catalogs (database 0), the block map's own files and every normal
  relation are md.c.
- The choice is recorded in files, so it needs no catalog access and works
  the same in backends, I/O workers, the startup process and tools: a
  block-mapped fork has a stub file (below). A backend checks for stubs
  only in databases that have a block map, which it learns once per
  database.
- Creating storage says which kind: `RelationCreateStorage` takes the
  relation's storage kind, which the `prolly` AM sets for its tables and
  TOAST tables, and index creation takes from the index's table.
- Step 1 keeps prolly relations to the default tablespace (a tablespace
  clause on one is ignored with a notice, as for D1's tables). Creating a
  database with the `FILE_COPY` strategy copies the block map's files with
  the rest after its checkpoint; the `WAL_LOG` strategy copies each prolly
  relation into the new database's block map.

### Files

Per database, created with its first prolly relation, in its directory:

| File | What | Accessed through |
|---|---|---|
| The page store (a reserved relation file number) | Every prolly relation's pages, at physical block numbers | md.c directly, never the buffer manager |
| The map (another reserved number) | Metapage, free-space bitmap, and each prolly fork's map tree | The buffer manager, as an ordinary md.c relation: WAL, full-page writes and checkpoints apply unchanged |
| `<relfilenumber>[_fork]_bm`, one per prolly relation fork | A stub: its map tree's root block. Written once when the fork is created | Files; their existence is the fork's existence |

The stubs keep everything that works on files at file granularity
(existence checks, unlink, directory scans) working, and their names
aren't relation-file names, so tools that verify checksums skip them.
The page store's pages carry checksums computed with their *logical*
block numbers (the buffer manager computes and verifies them), so tools
that verify checksums from files (`pg_checksums`, base backups) skip the
page store too; the map relation verifies normally.

### Maps

- A fork's map is a radix tree of map pages, its root fixed at creation
  (it records the fork's logical length and the tree's depth). Leaf
  entries are (physical block, birth epoch), internal entries (map page,
  birth epoch); about a thousand per page, so depth 2 covers 8 GB and
  depth 3 8 TB. Growing a level copies the root's entries to a new page.
- Epochs are unused in step 1 (no sharing); they are in the format now so
  step 2 can decide sharing without rewriting maps.
- Physical blocks are allocated from a free-space bitmap (the map's pages,
  listed by its metapage), preferring free blocks, otherwise extending the
  page store at its high-water mark. Reused blocks are zeroed before use,
  so a crash can't expose a page's old contents at a new logical block.

### WAL and crash safety

The map is replicated state, identical on a primary and its standbys
(as md.c's files are), so base backups and standbys work unchanged. Every map change is WAL-logged by our
own resource manager, registering the map pages it changes so torn map
pages are restored by full-page images:

- **Create** a fork: its root page and stub.
- **Extend**: allocation, leaf entries, the length, the bitmap and the
  high-water mark, logged before the new blocks are used, so a data
  record for a new block always follows its allocation. Redo zeroes the
  blocks again.
- **Truncate**: the length only, inside `RelationTruncate`'s critical
  section with the root pinned beforehand (a new smgr callback prepares
  truncation before the critical section). Blocks past the length are
  reused by a later extension or freed with the fork.
- **Drop**: after commit, the fork's map pages and blocks are freed by
  records of their own. A crash between the commit and these records
  leaks the blocks until reclamation (step 7) sweeps forks with no stub.

Redo of `XLOG_SMGR_CREATE` and `XLOG_SMGR_TRUNCATE` finds the map already
changed by the records above, which the primary always logs first.

### I/O

- Reads and writes translate logical runs to physical runs and call md.c
  on the page store; `smgrmaxcombine` reports the physically contiguous
  run, so the buffer manager's combined reads never cross a gap.
- Asynchronous reads are started on the page store's md.c relation, so an
  I/O worker reopening the target needs no map lookup.
- Sync requests are md.c's, for the page store's segments and the map's;
  `smgrimmedsync` syncs the page store.

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

- `pg_rewind` and `pg_upgrade` don't understand block-mapped databases yet.
- Freed blocks are reused, but the page store never shrinks.
- Map lookups cost a pinned map page or two per I/O; measured in step 8.
