# 11. Pages for the working set, Dolt-style history

**Status: proposed (user direction, 2026-10-15); not built.** This
replaces the design in which prolly trees are a table's primary storage
(engines D1–D4, `04-candidate-designs.md`). The decisions below marked
"user" were made on 2026-10-15; everything else is a recommendation to be
settled before the step that needs it. Part 4 is the plan for getting from
the code as it is today, open PRs included, to this design.

## 1. The design

### Two layers

- **The working set is pages.** A versioned database's tables, indexes,
  catalogs, sequences and TOAST tables are ordinary Postgres relations
  in 8 KB pages: heap, nbtree, GIN, GiST and the rest, unchanged. Reads,
  writes, MVCC, row locks, SSI, HOT, TOAST, VACUUM, index-only scans and
  parallel scans are Postgres's own, so performance and concurrency are
  heap's.
- **History is a chunk store shaped like Dolt's.** Commits are immutable,
  content-addressed chunks of prolly trees, kept the way Dolt keeps them:
  new chunks appended to a journal, older ones in immutable table files
  with an index, and a manifest naming the root. The formats are our own
  (user: no Dolt or Doltgres byte compatibility); Dolt is the model for
  structure and behavior, not for bytes.
- **A Dolt commit snapshots the working set into the store.** A SQL commit
  is not a Dolt commit: SQL commits only change pages; `dolt_commit()`
  folds the branch's changes since its head into new trees and a new
  commit.
- **There is no staging area** (user). A Dolt commit takes the branch's
  whole working set (or the tables it names, without a persistent staged
  state).

Both extremes are valid and cheap: a table that has only the initial Dolt
commit and only ever changes in its working set is a heap table in all but
name; a table where every SQL commit is also a Dolt commit keeps its delta
at one transaction's size.

### Branches are page sets, copy-on-write (user)

- Each branch of a versioned database has its own set of pages holding its
  head plus its uncommitted (in the Dolt sense) changes. Every relation of
  the database, the catalogs included, has a separate relation file number
  on each branch, and each branch has its own relation map (the
  relation mapper's file) for nailed and mapped catalogs.
- **Pages are shared copy-on-write.** Under each branch's relation files is
  a per-branch **block map** from logical block to physical block in a
  shared page store. Creating a branch copies the parent's maps by
  reference: no page is copied. The first logical change to a shared page
  on either branch gives that branch its own copy.
- Connections choose a branch by database name (`db/branch`, as built);
  the connection's catalogs, relcache and everything else see that
  branch's relations. DDL on a branch changes only that branch, because the
  catalogs are that branch's pages. This gives versioned schema without
  the staged catalog conversion of `09-versioned-catalogs.md` part 4.
- **Shared catalogs** (`pg_database`, `pg_authid`, tablespaces) stay
  cluster-wide and unversioned, as decided for question 27.
- A branch that no connection uses and that has no uncommitted changes can
  drop its page set; it is then only its head commit. Its pages are made
  again when it is next used: copy-on-write from a branch whose head is
  close to it, then the difference applied (part 2), or, when no branch
  is close, materialized from the tree.
- Temporary and unlogged relations are never versioned (as decided,
  question 28): they stay ordinary relations outside the block map.

### What is in pages and what is in the chunk store

| | Pages (working set) | Chunk store (history) |
|---|---|---|
| Format | Postgres's: heap tuples, btree, catalogs | Ours: prolly trees of canonical rows, canonical schema, root values, commits |
| Mutability | Mutable, MVCC | Immutable, content-addressed |
| Ordering | Postgres collations, opclasses | The canonical key encoding (memcmp order from btree opclasses) |
| Durability | WAL, checkpoints, full-page writes | WAL-logged appends to the store's files (below) |
| Per branch | Block maps (copy-on-write) | Refs (branch heads, working-set roots) |
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
  remain question 35; unlogged tables stay unversioned; btree and other
  index AMs are each branch's own, through the block map, so a TRUNCATE on
  one branch no longer affects another; merging into a branch with
  uncommitted changes is allowed when nothing conflicts, as in Dolt
  (part 2).
- **Versioned catalogs** come from per-branch page sets instead of
  catalogs on prolly trees: `09-versioned-catalogs.md` part 4's stages 2–7
  are superseded. Its part 1 (canonical commits, identity by name) carries
  over.
- **The 8 KB question:** pages are Postgres's; chunks are the store's and
  no longer depend on `BLCKSZ`.
- **Remotes** use our own protocol (V4); DoltHub and Dolt's remote API are
  out of scope.

## 2. Operations

| Operation | How | Cost |
|---|---|---|
| SQL read or write | Heap and indexes on the branch's pages | Heap's |
| Create a branch at another branch's head | Copy the block maps by reference; new relation file numbers and relation map | O(relations), aim O(1) with the maps under one copy-on-write root |
| Create a branch at a commit | As above from a branch whose head is near it, then apply the tree diff between the two heads as row changes | O(Δ log N) |
| Dolt commit | Fold: find rows changed since the head (below), write them into the head's trees, then the schema, root value, commit, ancestry and refs | O(changed pages + Δ log N + log H) |
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
| Storage manager (smgr) | Already a table of functions (`smgrsw[]`, `src/backend/storage/smgr/smgr.c`), with md.c the only entry | A second implementation, block-mapped copy-on-write storage, chosen per relation (versioned database, permanent relation); md.c stays the standard one |
| Buffer manager | None | One hook: the first `MarkBufferDirty` of a page whose physical block is shared remaps it before the change is WAL-logged; `MarkBufferDirtyHint` writes stay in place (hint bits don't change any branch's rows). Buffer tags stay logical (per-branch relation file numbers) |
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
| `dolt_*` functions, views, `dolt.author`, `db/branch` connections, conflicts as in Dolt, `CREATE DATABASE … VERSIONED`, pg_dump support (#25–#27) | Kept; their backend changes to page sets and the chunk store |
| Hash, key encoding, canonical rows, value trees, canonicalizer (4a, #18, #23) | Kept: the chunk store's contents |
| Copy-on-write trees, subtree-skipping diff, three-way merge, merge conflicts, refs, commits (4c, #9, #21, #22, #26) | Kept; they operate on chunks from the new store instead of node-store pages |
| Chunker, node format | Reworked: the byte-based rule (step 5's plan) and pageless nodes |
| Interface registry and stub pattern (`prolly_registry.c`) | Kept; the new interfaces register there |
| Requirements suite (`src/test/prolly`), `test_prolly`'s version-control tests, the benchmark harness, gates 1–7 | Kept; storage-engine tests go with D1 |
| The node store (`nodestore_global.c`), reclamation (#19) | Replaced by the journal and table-file store and its GC |
| D1 and K1 (table AM on trees, row locks, TID maps, keyings, spill, SERIALIZABLE at table granularity, the 4h fixes to them), the prolly index AM | Superseded by pages. Kept building until the new design is measured against them (step 8), then removed |
| `09-versioned-catalogs.md` part 4 stages 2–7, and #34 (catalog writes through the table AM) | Superseded by per-branch catalogs in pages; #34 stays harmless |

### Steps

Each step is a stack of PRs with its own tests (every bug with a test that
fails without its fix), and Postgres's suites keep passing after each one.

**Step 0. Settle the open stack and these specs.**
- Recommended: merge #12–#46 as they are, then tag the result
  (`prolly-d1`). They are tested, D1 is the measured alternative step 8
  compares against, and most of their version-control code and tests
  carry over. Rebasing the stack piecemeal would cost more than it saves.
  **Decision for the user** (merging needs approval).
- Mark this design in `README.md` and `08-open-questions.md`'s status
  table; mark what it supersedes in `04`, `07`, `09` and `10`.
- Decide question 35 (types without binary send formats) and whether
  shared catalogs stay unversioned.

**Step 1. The page-store interface and the block-mapped store.**
- Make smgr's implementation selectable per relation, md.c the default.
- A block-mapped implementation: per-relation-fork block maps (logical →
  physical, with a birth epoch per entry), a shared physical page file per
  database, allocation and free-space reuse, WAL records for map changes,
  redo, checkpoints, and AIO reads.
- The buffer-manager hook: remap a shared page on its first
  `MarkBufferDirty`.
- **Exit:** with every permanent relation of a database on the block-mapped
  store (one branch, no sharing yet), gates 1–3 match stock Postgres;
  crash and standby tests of the map pass.

**Step 2. Branches as page sets.**
- Branch creation copies block maps by reference; per-branch relation file
  numbers and relation maps; `db/branch` connections open the branch's
  relations and catalogs.
- Freeing a branch's page set past the xmin horizon; VACUUM and freezing
  on shared pages (visibility-neutral changes in place; line pointers made
  unused only when no other branch shares the page); wraparound across
  page sets.
- `dolt_branch` and `dolt_checkout` on page sets, no commits yet.
- **Exit:** DDL and TRUNCATE on one branch leave the others alone
  (`vc_schema` passes); branch creation reads no table's pages; the S
  suite on a branch matches heap.

**Step 3. The chunk store, WAL-logged.**
- The journal and its index, table files, the manifest and the refs map,
  in each versioned database's directory under the data directory;
  concurrent appends from backends serialized by a lock.
- The resource manager: chunk appends, the root in the commit record,
  table files, manifests, unlinks; redo and base-backup consistency.
- Pageless nodes and the byte-based chunker; the tree, diff and merge
  code reading and writing through the new store.
- **Exit:** the tree tests of `test_prolly` pass on the new store; crash
  tests at every record boundary recover to the last committed root; a
  standby's store matches the primary's byte for byte.

**Step 4. Dolt commits from pages.**
- The fold: the change finder, tree edits, the canonicalizer, commit and
  ancestry, refs and working-set roots; working-set checkpoints.
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
