# 11. Pages for the working set, Dolt for history

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
- **History is a Dolt store.** Commits are Dolt chunks, byte-compatible
  with what Doltgres writes: a chunk journal, table files and archives,
  and a manifest, in Dolt's on-disk layout (user). Dolt and Doltgres can
  read a versioned database's store, and DoltHub remotes can serve it.
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
  drop its page set; it is then only its Dolt head. Its pages are made
  again when it is next used: copy-on-write from a branch whose head is
  close to it, then the difference applied (part 2), or, when no branch
  is close, materialized from the tree.
- Temporary and unlogged relations are never versioned (as decided,
  question 28): they stay ordinary relations outside the block map.

### What is in pages and what is in Dolt

| | Pages (working set) | Dolt store (history) |
|---|---|---|
| Format | Postgres's: heap tuples, btree, catalogs | Doltgres's: TUPM trees of Doltgres-encoded tuples, DSCH schemas, DGRV root values |
| Mutability | Mutable, MVCC | Immutable, content-addressed |
| Ordering | Postgres collations, opclasses | Doltgres's comparators (binary collation) |
| Durability | WAL, checkpoints, full-page writes | WAL-logged appends to Dolt's files (below) |
| Per branch | Block maps (copy-on-write) | Refs (`refs/heads/<b>`, `workingSets/heads/<b>`) |
| Indexes | Every Postgres index AM | Primary and secondary index trees, as Doltgres stores them |

### Durability of the Dolt store (user)

Dolt's files are written in Dolt's formats, and every change to them is
WAL-logged through a resource manager of our own, so streaming
replication, base backups and point-in-time recovery carry the history
too, and a Dolt commit is atomic with its SQL transaction:

- **Chunk appends** are logged as the journal record bytes they append.
  An aborted transaction's chunks stay in the journal, unreachable, and are
  collected by GC, as Dolt does with chunks never referenced by a root.
- **The root moves with the commit record.** The new StoreRoot address
  (and the root record's timestamp) is carried by the transaction's commit
  record, so redo applies it exactly when the transaction committed.
  Replay writes the logged timestamp, never the clock, so a standby's
  journal is byte-identical to the primary's.
- **New table files and archives** (GC, fetch, clone) are logged as their
  chunks, or as file contents for large transfers; **manifest
  replacements** and **unlinks** as their own records.
- Dolt's own fsync points (the journal sync at each root record, the
  temp-file, fsync and rename of tables and manifests) stay, so a cleanly
  shut down directory is a valid Dolt database by itself; the WAL makes
  crash recovery and replicas exact.

### Byte compatibility: what it takes

From a reading of Dolt at `51cc4458` and Doltgres at `d5e05f46`
(2026-10-14):

- **Addresses:** the first 20 bytes of SHA-512 of a chunk; names in
  Dolt's base32. Our SHA-256 content hash is replaced.
- **Messages:** flatbuffers with Dolt's 4-byte prefix and file
  identifiers (STRT, ADRM, CMCL, DCMT, DGRV, DTBL, DSCH, TUPM, WRST, BLOB,
  ARTM, DTAG). Identical hashes need Go's flatbuffers builder's exact
  layout, so the C builders repeat Dolt's builder call sequences, padding
  and default omission, and are tested against chunks Dolt writes.
- **Trees:** Dolt's TUPM nodes (u16 offset vectors, 64 KB node cap,
  varint subtree counts), the `keySplitter` chunker (xxh3 of key bytes,
  salted by level, a Weibull rule between 512 bytes and 16 KB; Go's
  `math.Expm1` ported exactly), canonical root collapse, blob trees of
  fixed 4,000-byte leaves for out-of-line values, and Doltgres's tuple
  spill rule (a 2,048-byte target).
- **Tuples and values:** Dolt's tuple layout and encodings, and Doltgres's
  per-type serializations (native encodings for integers, floats,
  numeric, text, bytea and json; Doltgres's own formats for the rest),
  with comparators that order as Doltgres does.
- **Schema and catalog objects:** DSCH columns carry Doltgres's type
  descriptors (`"extended_" + hex`), which embed Doltgres's built-in type
  table and internal function ids byte for byte; sequences, types,
  functions, triggers and extensions are root objects keyed by Doltgres's
  `id.Id` (identity by name, as decided for this project).
- **Refs:** branches, tags and working sets are entries of the StoreRoot
  address map, the manifest naming the StoreRoot. The WorkingSet message
  has a staged root; with no staging we write the staged root equal to the
  head's root.

### Where Doltgres's format can't hold Postgres's data

Byte compatibility must not lose data or refuse ordinary Postgres types.
The reading found these gaps:

1. **Types Doltgres doesn't store:** inet, cidr, macaddr, money, the
   geometric types, range and multirange types, tsvector, tsquery,
   pg_lsn, txid_snapshot, aclitem, several reg* types, and extension types
   other than uuid-ossp's and pgvector's.
2. **Lossy formats:** json and jsonb go through Go's `encoding/json`
   (numbers become float64, json's text and key order aren't kept);
   timestamps carry the zone offset of the Go value that wrote them, so one
   instant can have several encodings.
3. **Functions and triggers** are stored as Doltgres's compiled PL/pgSQL
   opcodes, which a C writer can't produce without porting Doltgres's
   compiler.
4. **Collation:** Doltgres orders text by binary collation and ignores
   `COLLATE`. This only affects the Dolt trees' order, not Postgres's
   indexes, so it is acceptable (and removes question 46's collation
   versions from the stored format).

**Recommended:** close the gaps in Doltgres's format itself, as new
encodings or new versions of its serializers, rather than diverging from
it or losing data: Postgres's binary send/receive formats for the missing
types; json as its exact text; jsonb in a lossless canonical form; one
canonical zone for timestamps; function and trigger source text alongside
(or instead of) opcodes. Until a change lands in Doltgres, we write the
proposed format under its new version number, so nothing is lost and
Doltgres refuses rather than misreads it. **Decision for the user.**

### What this answers

- **Step 4h's pending decisions:** types without binary send/receive
  (decision 15) become the gap list above; unlogged tables stay
  unversioned; btree and other index AMs are each branch's own, through the
  block map, so a TRUNCATE on one branch no longer affects another; merging
  into a branch with uncommitted changes is allowed when nothing conflicts,
  as in Dolt (part 2).
- **Versioned catalogs** come from per-branch page sets instead of
  catalogs on prolly trees: `09-versioned-catalogs.md` part 4's stages 2–7
  are superseded. Its part 1 (canonical commits, identity by name) carries
  over, now in Doltgres's schema format.
- **The 8 KB question:** pages are Postgres's; chunks are Dolt's (512 bytes
  to 16 KB) and live in Dolt's files, not in pages, so the chunk rule no
  longer depends on `BLCKSZ` (question 45).

## 2. Operations

| Operation | How | Cost |
|---|---|---|
| SQL read or write | Heap and indexes on the branch's pages | Heap's |
| Create a branch at another branch's head | Copy the block maps by reference; new relation file numbers and relation map | O(relations), aim O(1) with the maps under one copy-on-write root |
| Create a branch at a commit | As above from a branch whose head is near it, then apply the tree diff between the two heads as row changes | O(Δ log N) |
| Dolt commit | Fold: find rows changed since the head (below), write them into the head's trees, then the schema, root value, commit, closure and refs | O(changed pages + Δ log N + log H) |
| Working-set checkpoint | The same fold, published as the WorkingSet's working root instead of a commit | As a commit |
| Diff, log, status | Over Dolt trees: subtree-skipping diff; working-set diffs fold first | Dolt's bounds |
| Fast-forward, reset, checkout of a commit | Apply the tree diff from the current head to the target as row changes | O(Δ log N) |
| Three-way merge | Merge trees as Dolt does (base, the target branch's folded working set, the source), then apply the diff from the target's working set to the result as row changes; conflicts become artifacts and `dolt_conflicts` rows | Dolt's merge bound plus O(Δ log N) |
| Fetch, push, clone, pull | Dolt's remote protocol over the store; a cloned branch gets pages when first used | Dolt's bounds; first use O(N log N) materialization |

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

**What Dolt tools see.** Branches, tags and commits are exact. A working
set is visible to Dolt as of the last fold (a commit, a checkpoint or a
merge); changes since then live only in pages.

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
| full_page_writes, WAL | Generic | Unchanged for pages; new records for block-map changes and the Dolt store |
| bgwriter, checkpointer | Generic over smgr | The checkpoint also flushes the block maps and syncs the Dolt journal |
| pg_stat_io | Generic | Unchanged |

Each new piece sits behind an interface with a stub and an implementation,
as `00-architecture-principles.md` requires: the page store (md.c, block-
mapped), the branch page-set manager, the change finder, the row applier,
the Dolt chunk store (journal and table files), and the format layers
(messages, trees, tuples, value encoders).

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
| `dolt_*` functions, views, `dolt.author`, `db/branch` connections, conflicts as in Dolt, `CREATE DATABASE … VERSIONED`, pg_dump support (#25–#27) | Kept; their backend changes to page sets and the Dolt store |
| The interface registry and stub pattern (`prolly_registry.c`) | Kept; the new interfaces register there |
| Subtree-skipping diff, three-way merge, merge conflicts (#9, #22, #26) | Algorithms kept, rewritten over Dolt's node format |
| Canonicalizer: identity by name, schema from catalogs (#23) | Kept in purpose; its output becomes Doltgres's DSCH and root objects |
| Requirements suite (`src/test/prolly`), `test_prolly`'s version-control tests, the benchmark harness, gates 1–7 | Kept; storage-engine tests go with D1 |
| D1 and K1 (table AM on trees, row locks, TID maps, keyings, spill, SERIALIZABLE at table granularity), the prolly index AM, the node store, our node format, SHA-256, the item-count chunker, value trees, `valenc_canonical` | Superseded. Kept building until the new design is measured against them (step 9), then removed |
| `09-versioned-catalogs.md` part 4 stages 2–7, and #34 (catalog writes through the table AM) | Superseded by per-branch catalogs in pages; #34 stays harmless |

### Steps

Each step is a stack of PRs with its own tests (every bug with a test that
fails without its fix), and Postgres's suites keep passing after each one.

**Step 0. Settle the open stack and these specs.**
- Recommended: merge #12–#46 as they are, then tag the result
  (`prolly-d1`). They are tested, D1 is the measured alternative step 9
  compares against, and #25–#27's SQL interface and tests carry over.
  Rebasing the stack piecemeal would cost more than it saves.
  **Decision for the user** (merging needs approval).
- Mark this design in `README.md` and `08-open-questions.md`'s status
  table; mark what it supersedes in `04`, `07`, `09` and `10`.
- Decide the format gaps above (part 1), and whether shared catalogs stay
  unversioned.

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
- `dolt_branch` and `dolt_checkout` on page sets, no Dolt store yet.
- **Exit:** DDL and TRUNCATE on one branch leave the others alone
  (`vc_schema` passes); branch creation reads no table's pages; S suite on
  a branch matches heap.

**Step 3. Dolt's formats in C.**
- SHA-512/20 addresses and base32; a flatbuffers builder that reproduces
  Dolt's layout; the messages; TUPM, ADRM and CMCL trees with the
  `keySplitter` chunker; blob trees; tuples and Dolt's encodings;
  Doltgres's value serializers and comparators; DSCH type descriptors from
  Doltgres's built-in type table.
- The format gaps' new encodings, as decided in step 0.
- **Exit:** a golden corpus written by Doltgres (every supported type,
  keyless tables, large values, schemas, commits, branches) is read
  exactly, and rewriting its logical content produces the same chunk
  addresses.

**Step 4. The Dolt store on disk, WAL-logged.**
- The journal and its index, table files and archives (read and write),
  the manifest, in each versioned database's directory under the data
  directory; concurrent appends from backends serialized by a lock.
- The resource manager: chunk appends, the root in the commit record,
  table files, manifests, unlinks; redo and base-backup consistency.
- **Exit:** a store copied out of a stopped server, or a standby's copy, is
  opened by `dolt` and Doltgres (`dolt log`, `dolt fsck`); crash tests at
  every record boundary recover to the last committed root.

**Step 5. Dolt commits from pages.**
- The fold: the change finder, tree edits, the canonicalizer writing
  Doltgres's schema and root objects, commit and closure, refs and
  working sets; working-set checkpoints.
- `dolt_commit`, `dolt_log`, `dolt_status`, `dolt_diff` and the views on
  the new store.
- **Exit:** gate 6 (history independence) and the requirements suite's
  commit and history tests pass; a commit's root equals the root Doltgres
  writes for the same content (cross-implementation hash test); commit
  cost tracks changed pages.

**Step 6. Moving heads.**
- The row applier (from logical replication's apply worker); checkout and
  reset to a commit, fast-forward, branch at a commit, materializing a
  branch with no pages.
- Three-way merge over Dolt trees into a working set with changes;
  conflicts as artifacts and `dolt_conflicts`; cherry-pick, revert.
- **Exit:** `vc_branches`, `vc_diff_merge`, `vc_history` and the
  conflicts tests pass; V1–V3 run, V3 with background writes.

**Step 7. Remotes.**
- File remotes first, then Dolt's remote API: fetch, push, clone, pull;
  a cloned branch materialized on first use.
- **Exit:** V4; clone from DoltHub of a database Doltgres wrote, and
  Doltgres clones of ours.

**Step 8. Reclamation.**
- Dolt GC with WAL: archives, `oldgen`, journal retirement; page-set and
  physical-page reclamation; the fold epoch's retained page versions.
- **Exit:** space returns after branch deletion and GC; crash tests of GC.

**Step 9. Measure, then retire D1.**
- Gates 1–7 and the S and V suites for this design against stock heap and
  against D1; `10-complexity-requirements.md` rechecked operation by
  operation.
- Then remove D1, K1, the prolly index AM, the node store and the
  superseded format layers, in their own PRs, and update the specs.

### Risks to retire early

- **Shared pages and VACUUM.** Pruning and freezing on a page another
  branch shares must not change either branch's rows or break the other
  branch's index entries. Step 2 settles the rules before anything
  depends on them.
- **Flatbuffers layout.** Byte-identical messages depend on Go builder
  behavior; step 3's golden corpus is the test, and is built first.
- **Doltgres's format changes.** The gaps are changes to Doltgres; they
  need agreement with the Doltgres maintainers before step 3 writes them.
- **Two buffers for one physical page** (one per branch sharing it) cost
  cache, not correctness; measured in step 9.
