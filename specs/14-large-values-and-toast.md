# 14. Large values: keeping TOAST, versioned

**Status: proposed 2026-10-17, on `64c9001f9a`; revised from review the same day. Decided by the user the same day, as recommended: the `oid` refusal and B are built before step 8's measurements, C after them (part 8), with C7's questions 1 and 2 decided. The refusal and B built 2026-10-17 and merged 2026-10-18 (#217–#221, #224, one stack); B is complete (#180 closed 2026-10-18), with what differs from the design marked "Not as designed" under "As built" in parts 5 and 7. C's questions 3 and 4 and how to build it decided by the user 2026-10-18, as recommended (C7). Gates 1 and 3 measured on a throwaway prototype and round 2 decided by the user the same day (C8): no hybrid, the journal's append reworked first, C built as C8's layers. C isn't built (#181). Code references refreshed on `8faba578f9`.**
This compares how stock Postgres, Dolt and versioned prolly tables store
large values, and proposes two steps that keep as much of TOAST as possible
while Dolt's invariants hold: B, stock TOAST in the working set with
history that stops paying for it (#180); and C, TOAST pointers that name
content in the chunk store (#181). B is the near-term work and C the
target; C reuses what B builds. Large objects (`pg_largeobject`) are a separate mechanism
and are only touched in part 7.

## 1. How each one stores a large value

| | Stock Postgres (TOAST) | Dolt (adaptive encoding) | Prolly history today |
|---|---|---|---|
| What decides | Per row, per column: the toaster runs when a row is over 2032 bytes and works down to `toast_tuple_target` | Per row, per column: a row over `target_row_size` (2048) moves columns out | Per row, the whole row: an encoded row over `VALUE_INLINE_MAX` (2048) goes out of line; since B, per column: a column whose canonical bytes are over 2048 goes out of line on its own, the whole row only if it is still long |
| Which column first | The largest eligible (`toast_tuple_find_biggest_attribute`), in four passes by storage strategy | The one saving the most bytes | — |
| Compression | pglz or lz4, per column (`attcompression`), before moving out | None | None: canonical bytes are uncompressed (question 48) |
| Out-of-line form | Rows of `pg_toast_N (chunk_id, chunk_seq, chunk_data)`, 1996-byte chunks, value named by an OID | A blob tree, fixed 4000-byte leaves, named by its hash | A value tree, buzhash chunks of 512–4096 bytes keyed by byte offset, named by its hash (`tree_cow.c:560-608`); since B, addressed by byte counts and one per long column (part 5) |
| Sharing | None: one owner per value; an unchanged value keeps its pointer on UPDATE | Identical values are one tree | Identical rows are one tree; since B, identical long values are one tree wherever they are, and an apply in place keeps a row's own TOAST pointer for a value that didn't change |
| Slice reads | Uncompressed values fetch only the chunks they need; pglz fetches a prefix | None | None |
| Deletion | With its row; VACUUM of the TOAST table reclaims | `dolt gc` | No chunk-store reclamation at first; since #210–#225, a collection of the chunk store, which keeps every value tree a leaf names |

The versioned working set is heap pages with stock TOAST: a versioned
table's TOAST table and its index are block-mapped and branch-aware
(`heapam_handler.c:489-506`, `versioned.c:164-169`), and branches keep the
same TOAST relation OID with copy-on-write storage, so a pointer resolves
on the session's branch (part 11, lines 78-90). Folding a row into history
decodes every column: `CODEC_TEXT` through `DatumGetTextPP`, other types
through their send functions (`prolly_datum.c:176-191, 594-618`); jsonb's
send form is its text. No TOAST pointer or value OID reaches history, and
TOAST relations are never folded themselves (`canon_folded.c:1786-1788`).

## 2. What has to hold

1. A commit's and a root's hash are a pure function of the logical rows:
   no OIDs, no compressed bytes, no choice of where a value was placed.
2. History is immutable and content-addressed, so equal values share
   storage across rows, versions and branches.
3. Fetch, push and reclamation reach every chunk from a root, without the
   local catalog.
4. Diff and merge cost is proportional to the change; equal values compare
   by hash.
5. Branches are isolated, and a SQL rollback undoes everything a
   transaction wrote, its stored values included.

## 3. Rejected: folding the TOAST table

Folding `pg_toast_N` as an ordinary versioned table keyed by
`(chunk_id, chunk_seq)` keeps 100% of TOAST and breaks invariants 1 and 4:

- Value OIDs enter history: two databases with the same rows hash
  differently, and merge would have to remap OIDs.
- Compressed bytes enter history: a different lz4 build changes hashes.
- An unchanged value re-toasted under a new OID is a change in diffs.

## 4. Today's costs

1. **Whole-row value trees.** A row with an int and a 10 KB text is one
   out-of-line value. Diff and merge resolve the row to see the int, and
   two rows sharing the text share nothing unless the bytes before it agree.
2. **Value trees keyed by offset.** A small column growing by one byte
   shifts every offset after it and rewrites the rest of the tree (roadmap
   4f.1, "Not yet").
3. **Every fold decodes.** A committed UPDATE of a small column decodes,
   decompresses and rehashes the row's large columns, though TOAST kept
   their pointers unchanged.
4. **Every apply re-toasts.** Merge, reset and branch-at-commit write rows
   through the applier (`applier_replication.c:574-688`) as decoded datums,
   so the heap compresses and stores every large value again, under a new
   OID, even where the target row already holds the same value.
5. **jsonb is stored as text.** Large documents round-trip through
   `jsonb_out` and `jsonb_in`.
6. **Old out-of-line keys force a full fold.** An old page version whose
   key column (or, keyless, any column) is out of line makes the commit
   fold the whole table (`changes_pages.c:450-499, 766-785`). B and C leave
   this as it is.

## 5. B: stock TOAST in the working set, history that stops paying for it

TOAST is unchanged: thresholds, passes, storage strategies, compression,
slice reads, VACUUM. History changes how it encodes large values, and
fold and apply stop decoding what hasn't changed.

### B1. A reference per column

The canonical row encoding (`valenc_canonical.c`) stores a column whose
canonical bytes exceed `T` as a reference to its own value tree:
a tag, the tree's root hash and the value's length, as `encode_value_ref`
does now for rows. The rule is per value and fixed by the format, never
"what TOAST did" or "what the rest of the row needs", so the same value
encodes the same way in every row (invariant 1). `T` is 2048 to start,
recorded in the format; Dolt's per-table `target_row_size` is not copied,
because a per-table rule would make one value's encoding depend on its
table.

The whole-row reference in `tree_cow.c:645` stays as the fallback for a
row of many medium columns that is still over the node limit.

The canonical format version goes up; history written by the old format
stays readable. Across the boundary the same rows hash differently, so a
diff or merge between roots of different formats can't skip equal subtrees
by hash: it compares those tables row by row by decoded content, at a cost
proportional to the table once, and a merge whose base is in the old format
refolds the base in the new one first. (The alternative is #149's: a clean
break, older roots' tables not read. It is simpler, and right only while
nobody has history worth keeping.)

**As built (#219).**
- `T` is 2048 (`valenc_canonical.c`). A reference's length prefix is
  2048 + 1 + the reference's length, which no inline value's can be, then
  the reference (the tree's: hash length, hash, value length). A row
  without long values is the same bytes as before, so `format_freeze`'s
  pinned trees changed only for its table of large values.
- References are written and read through the tree interface's new
  `value_write()` and `value_read()`. The value encoder gets them from a
  value store (`ProllyValueStore`: the tree, its environment, and B3's
  memo) given by `set_values()`; the keying passes it on, and
  `prolly_keying_use_env()` sets a data tree's environment and a keying up
  for each other. Encoding or decoding a long value without a store is an
  error. Every keying that encodes or decodes rows is set up (fold, diff,
  merge, apply, `dolt_diff`, rows read as defined now).
- Walks find per-column references without the catalog (invariant 3;
  the design didn't say how): a tree environment gets an optional
  `value_refs()`, which the keying's `row_refs()` answers as each item is
  put, and a leaf item that holds references leads with them, flagged
  `PROLLY_ITEM_VALUE_REFS`. Fetch, push, clone and reclamation follow
  them from the nodes alone, as do a walk's prefetch hints and the node
  diffs (`diff_nodes()`) network transfers send, and the tree's
  `references()`, by which the chunk store checks a chunk's references
  are stored before it stores it (a collection keeps whatever a chunk
  names); reads return the value without them. `large_values` collects
  the whole store and reads the long values back from history.
- **Not as designed:** the format change is a clean break, #149's, not
  the cross-format comparison above (the user's choice for this work):
  the value encoder's format goes to 2 and the folded roots' `!:format`
  to 5 (4 was B2's, after #195's 3), and a root of an older format has
  its tables listed, their rows not read, and is neither moved to nor
  merged.
- Measured (`large_values`): with a 960 KB document in a row, the commit
  of an UPDATE of the row's int adds 3 nodes (the commit, its root, the
  table's leaf) and none of the document's tree; copying the document
  into another row and another table adds 4.

### B2. Value trees addressed by byte counts

Value-tree leaves hold chunk bytes with no key; internal nodes record
each child's byte count, as Dolt's blob trees do. Chunk boundaries stay
buzhash. An insertion then rewrites only the chunks around it and the path
above them, and a slice is found by descending through byte counts (C uses
this for `substr`). Existing tests to extend: `large_values` (a one-byte
insertion in the middle writes O(depth) nodes) and
`016_d1_large_values.pl`'s cases moved to versioned tables.

**As built (#218).** Each buzhash chunk is the one item of a leaf node,
with no key; an internal node's items have no key either and record each
child's reference and byte count. The levels above the leaves are cut by
the chunker, given each child's hash as its item's key, so a changed
chunk moves at most the boundaries next to it. `value_read()` reads a
slice by descending through the byte counts, O(depth + its length)
nodes (the tree conformance check `large_values` checks slices at every
kind of offset and length). The tree's format version goes to 2.
Measured: a one-byte insertion or deletion in the middle of a 3 MB value
writes at most 8 nodes (conformance, which holds it to 8); three bytes
inserted into a 192 KB value of a versioned table committed 8 new nodes
when measured (`large_values`, SQL, which holds it under 20).
D1's `016_d1_large_values.pl` cases are `057_large_values.pl` on
versioned tables (#221): through a crash, and on a standby through
`dolt_diff`.

### B3. A memo from TOAST pointers to content

A fold that finds an on-disk TOAST pointer looks it up in a memo:

    (toast relation OID, value ID, xmin of chunk 0) -> (root hash, length)

and on a hit writes the reference without decoding. On a miss it decodes
as today, builds the tree, and fills the memo.

The xmin is in the key because a value ID alone isn't a name for content:
`GetNewOidWithIndex` only checks the current branch's TOAST index, so after
VACUUM removes a value, the same ID can be given to new content, and two
branches can each give one ID to different content. A value inserted
before a fork has the same chunk 0 (and so the same xmin and content) on
both branches; one inserted afterward gets its inserting transaction's
xmin. Checking a hit costs one index probe of the TOAST index for chunk 0,
against decoding and hashing the value.

The memo is per database in shared memory, bounded, and may be lost at a
restart: a miss is only slower. Persisting it is a later choice.

The memo names chunks a fold wrote, so it is filled only with chunks that
are durable and visible:

- **Filled at commit.** A fold's new entries are kept in backend-local
  memory and published to the shared memo when its transaction commits
  (after the chunk store's root moves); an abort discards them. Otherwise a
  rolled-back `dolt_commit` would leave entries naming chunks no one can
  read, and the next fold that hits one would write a dangling reference.
- **Checked on a hit.** A hit also asks the chunk store whether it has the
  root hash, which is cheap against decoding and catches anything else
  that removed the chunks (reclamation, part 6).
- **Never across a wraparound.** The key's xmin is the raw xmin of chunk 0
  (kept by freezing), which repeats after 2^32 transactions; each entry
  records the full transaction ID it was filled in, and entries filled more
  than 2^31 transactions ago are dropped, so a reused value ID with a
  reused xmin can't match an old entry.

**As built (#220).** The memo is an interface of its own, `value_memo`
(`prolly_memo.h`, interface 28), part of the value store, so the
canonical encoder looks a column's on-disk TOAST pointer up before
reading it and remembers the reference it made on a miss. Implementation
"shared" (`memo_shared.c`): 8192 entries in shared memory under a
spinlock, eight probed per key, the oldest replaced; the key also holds
the database and the column's type (whose encoding made the reference).
Entries are kept in the backend, one per key and at most as many as the
shared table holds, until `XACT_EVENT_COMMIT`, and forgotten on an abort,
on the abort of the subtransaction that made them (or of one it was
released into), at `PREPARE` (they aren't published at `COMMIT
PREPARED`: the next fold misses those values once), and when a parallel
worker ends (its leader may still abort); a hit asks the chunk store for
the tree's root (the tree's new `value_present()`), and an entry of the
transaction's whose tree is gone is dropped.
- **Not as designed:** a value whose first chunk is frozen when it would
  be remembered isn't. The 2^31 rule alone leaves a case: a first chunk
  whose xmin is already more than 2^31 transactions old when its entry is
  filled could see its value removed and another given its ID and the
  same raw xmin while the entry lives. An unfrozen xmin is younger than
  2^31 transactions, so it can't; a frozen one's age is unknown. Folds
  read frozen values every time.
- As designed, values TOAST kept inline compressed aren't memoized (they
  have no pointer): their cost 3 stays.
- Measured (`large_values`, three 96 KB bodies stored EXTERNAL): the
  commit after an UPDATE of the rows' counter hits all three and misses
  none, and adds 3 nodes; a commit rolled back leaves its values to be
  read again; a frozen value misses at every fold.

### B4. Apply keeps a row's own pointer

When the applier updates a row and a large column's new reference equals
the current row's (by B3's memo, or by the current row's reference in the
baseline tree), it passes the current row's datum through. `toast_tuple_init`
then sees identical pointer bytes and keeps the value (`toast_helper.c:78-101`):
no decode, no compression, no new OID.

This works for an UPDATE of the same row only. Apply paths that insert a
row rather than update one still re-toast: reset or merge into a table
that doesn't hold the row, branch-at-commit (which writes every row), and
the materialized-view path, which deletes and reinserts (the delete
schedules the old value's chunks for removal, so the new row can't keep
its pointer). Stock TOAST gives each value one owner:
deleting a row deletes its TOAST chunks (`heap_toast_delete`), so giving
another live row the same pointer would leave that row dangling when the
first is deleted. Sharing across rows needs C.

**As built (#221).**
- **Not as designed:** the applier didn't update rows. A reset,
  fast-forward, merge, cherry-pick or revert (everything that writes
  rows through `apply_rows()` or `merge_table()`) removed every changed
  row and inserted its new version, all removals first, so a unique constraint both sides satisfy
  isn't broken on the way (two rows swapping a value). Now, where a table
  has no unique or exclusion index but its primary key's (nothing to
  swap), a keyed row that changed is updated in place; otherwise it is
  written as before, its values stored anew.
- The comparison is by the rows' references, not the memo: the current
  row as the branch folded now and the row written. The value encoder's
  new `decode_kept()` (the keying's `decode_row_kept()`) leaves each
  column out of line with the same reference in both unread and marks
  it, and the applier's `update()` takes a keep mask and gives those
  columns the found row's stored values. The old version, which only
  finds the row, is decoded with every column out of line unread, whole
  if one of those is in the primary key. A materialized view has no
  primary key, so it is never updated in place.
- A row found by its key that a transaction the move's snapshot doesn't
  see wrote since the fold (a concurrent update) isn't the row the move
  expects: the move fails, to be retried, rather than keep that
  transaction's value (`057_large_values.pl`, by an injection point).
- A merge's in-place update that breaks a constraint while violations
  are recorded is rolled back in its subtransaction, leaving the row as
  it was.
- Measured (`large_values`): a reset to a commit that differs in a
  counter only, and a merge of a counter change, leave the rows' bodies
  their TOAST chunk IDs; a table with another unique index gets new ones;
  a body that changed is stored anew.

### B results

- Invariants: all hold; history never sees TOAST.
- Fixes cost 1 and 2 fully, 3 for unchanged values, 4 for rows whose large
  value didn't change.
- Leaves: a new large value is hashed once at commit; large values are
  stored twice (compressed in TOAST, in chunks compressed only once a
  table file holds them); historical reads have no slice reads; merge
  still re-toasts a value that moves between rows.
- Not tested yet: that rows whose values TOAST stored differently
  (another `SET COMPRESSION`, another `toast_tuple_target`) give
  identical commit hashes. Invariant 1 says they must; a test of it is
  owed.
- **As measured (#217–#221):** cost 1 (`large_values`: an int's update
  next to a 960 KB document commits 3 nodes; copies share the document's
  tree) and cost 2 (an insertion committed 8 nodes when measured; the
  SQL test holds it under 20, the tree's conformance check a one-byte
  edit of a 3 MB value to 8) are fixed; cost 3 for
  values TOAST stored out of line whose first chunk isn't frozen; cost 4
  for keyed rows of tables without another unique or exclusion index.
  Cost 5 (jsonb as text) and 6 are untouched, as designed.

## 6. C: TOAST pointers that name content

### C1. The pointer

A new on-disk tag per hash algorithm, `VARTAG_ONDISK_SHA256` (5), next to
`VARTAG_ONDISK_OID` (18) and `VARTAG_ONDISK_OID8` (4), added the way
`6e58d6356f` added oid8 (`VARTAG_SIZE` gets a pointer's size from its tag
alone, so a tag has one fixed size):

    int32  va_rawsize    -- as now: original size, header included
    uint32 va_extinfo    -- as now: stored size and compression method
    uint8  va_hash[32]   -- the value tree's root, the store's hash

42 bytes as a datum. It names no relation: a value is the same value on
every branch and in every table.

`VARATT_IS_EXTERNAL_ONDISK` does **not** include it: its 27 uses in 16
files assume a value ID and a TOAST relation. A separate predicate,
`VARATT_IS_EXTERNAL_ONDISK_HASH` (true for every hash tag), is tested where a hash pointer is
meant, and every `_ONDISK` use is audited before C is built. Known ones
that must learn the new tag or refuse it:

- `pg_column_toast_chunk_id` (`varlena.c`): no chunk ID; returns NULL.
- `reorderbuffer.c`: reassembles values from TOAST chunks; a hash pointer
  is fetched from the chunk store instead (C4).
- `toast_compression.c`, pageinspect, test_decoding, and
  `changes_pages.c`'s out-of-line key checks.
- `toast_tuple_find_biggest_attribute` (`toast_helper.c`): it sizes the
  pointer a value would become from the TOAST relation's ID type (18 bytes
  for OID); with a chunk-store destination it must use 42, or values of
  24–48 bytes are moved out into a pointer bigger than themselves.

### C2. Saving and fetching

Saving: `toast_save_datum` asks the table AM for a TOAST destination. A new
optional table AM callback, `relation_toast_save(rel, value) -> pointer`,
used when present, builds the value tree as B2 does (from the uncompressed
bytes, so the hash is B1's reference) and writes its chunks to the chunk
store. Stock heap tables never take the new path; their only cost is a
tag check in `detoast.c`.

Where the callback lives was open (decided 2026-10-18, C7). Returning a
copy of `GetHeapamTableAmRoutine()` with it set breaks the code that recognizes
heap by its routine's address: `heap_getnext` (`heapam.c:1451`, "only heap
AM is supported") and `REPACK (CONCURRENTLY)` (`repack.c:994`), both of
which prolly tables (and their TOAST tables) pass today. Either those two
checks learn to accept the prolly routine (they check the routine, not the
OID, precisely so that AMs reusing heap's handler work), or the
destination is chosen outside `TableAmRoutine`, by a toaster hook that
looks at the relation's access method, as block-mapped storage is chosen.
Recommended, and decided: the hook. (Spec 15's transaction commits wrap
prolly's routine anyway, for its write paths, and so make those two
checks accept it; the hook keeps the TOAST destination out of the
routine all the same.)

Fetching: `detoast.c` dispatches the new tag to a registered fetch hook,
whole or by slice. Slices descend B2's byte counts and read only the
chunks they need, for compressed and uncompressed values alike, since the
tree holds uncompressed bytes.

The toaster's passes keep their order and thresholds, with these changes
for a relation with a chunk-store destination:

- **Every check for a TOAST relation.** Passes 1, 2 and 4 test
  `reltoastrelid != InvalidOid` (`heaptoast.c`), and `toast_save_datum`
  opens `reltoastrelid` directly; all of them ask "has a TOAST destination"
  instead, and `toast_save_datum` doesn't open a TOAST relation for a
  chunk-store save. A prolly table needs no TOAST relation then (C6), and
  without these, pass 1 never moves out an EXTENDED value still too big
  after compression, and pass 4 never moves out MAIN values: "row is too
  big".
- **No compress-then-decompress.** A value moved out is stored
  uncompressed (the tree is built from uncompressed bytes), and one that
  stays inline stays compressed. So the toaster keeps an EXTENDED value's
  uncompressed datum beside the compressed one until the column is settled,
  and `relation_toast_save` gets the uncompressed bytes: one compression for
  a value moved out, not a compression and a decompression.
- **Hash pointers pass through.** `toast_tuple_init` fetches back and saves
  again any external value the row doesn't own: every external value of an
  INSERT, and of an UPDATE any that differs from the old row's
  (`toast_helper.c:105-145`). A hash pointer has no owner, so it is kept as
  it is: written into a row it costs nothing, whichever row it came from.
  Without this, C3's apply would detoast, compress, decompress and rehash
  every value it writes.

### C3. What it gives

- **Fold without decoding.** A hash pointer's hash is B1's reference for
  every type whose on-disk bytes are its canonical bytes: bytea anywhere,
  and text, varchar and bpchar in a UTF-8 database only. In other
  encodings those types' canonical bytes are UTF-8, converted through their
  send functions (`prolly_datum.c:254-259`), so their trees are built over
  the canonical bytes, not the on-disk ones: the toaster converts before
  building the tree, and the pointer's hash is then still B1's reference.
  (Otherwise the same rows would hash differently in a LATIN1 and a UTF-8
  database.) A small value TOAST moved out (the
  row was big) is read and stored inline, which is cheap. A large value
  that stayed inline compressed is built into a tree.
- **Apply without re-toasting.** The applier writes the reference as a
  hash pointer, for any row: values have no owner, so sharing across rows
  and branches is safe (B4's limit goes away).
- **Dedup everywhere.** Equal values in the working set, across rows,
  branches and history, are one tree.
- **Unchanged-value reuse on UPDATE** is by content, not by pointer bytes.

### C4. What it costs

- **Reclamation moves from VACUUM to the chunk store.** `toast_delete_datum`
  does nothing for hash pointers. The chunk store has reclamation since
  spec 11's step 7 (part 8 there; `reclaim_copying.c`, `dolt_gc()`, and
  autovacuum starting it): a copying collector from the refs' roots,
  with chunks newer than the collection's start kept and every appended
  batch's references checked. What C adds to it is roots outside the
  refs: every branch's working-set heap, scanned for hash pointers, and
  the chunks replication slots may still decode (below). Working-set
  churn (a 1 MB document updated a thousand times between dolt commits)
  is garbage until a collection. Dolt has the same property. Two things
  the refs' roots alone miss:
  - **Chunks a writer starts using again.** Content dedup means a writer
    can reference an old chunk set without appending anything: a value
    whose tree already exists, or an apply copying a reference. If marking
    has already passed every tuple that held it, the sweep frees it under
    the new row. So while a collection runs, every hash a writer puts or
    references (a save that finds the tree present, an apply writing a
    hash pointer, a fold) is added to the collection's keep set, as Dolt's
    GC safepoint does, and the sweep keeps the keep set's closure.
  - **Standby queries.** Heap TOAST is protected on a standby by recovery
    conflicts: pruning a tuple conflicts with snapshots that could still
    see it, or is held back by `hot_standby_feedback`. Chunk-store
    reclamation has no such conflict of its own. So marking reads every
    tuple still on a page, dead or alive, not just visible ones: a chunk
    set is freed only after VACUUM has pruned every tuple that named it,
    and that pruning's WAL, replayed before the collection's, already
    conflicted with (or was held back for) every standby snapshot that
    could read those tuples. Reclaiming history's own chunks (unreachable
    commits) has its own horizon, part 8 of spec 11.
  - **Logical decoding.** A collection keeps every chunk appended after
    the oldest replication slot's position, so a value a slot may still
    decode is there to fetch.
- **Compression moves below the hash.** Values out of line lose TOAST's
  compression; the chunk store's table files compress chunks with LZ4
  (spec 11, step 3), and C relies on that, keeping hashes over
  uncompressed bytes. The journal, where a transaction's chunks land
  first, holds them uncompressed until its chunks move to a table file.
- **Write throughput.** Chunk-store appends serialize on one
  `ProllyJournalLock` and flush WAL per batch (`logged_write()` and
  `append_bytes()`, `chunkstore_journal.c:669-778`). A TOAST-heavy insert
  load goes through it.
- **Logical decoding.** The reorder buffer reassembles TOAST values from
  the TOAST table's WAL. Hash pointers have no TOAST table rows; decoding
  fetches from the chunk store, which is safe because chunks are immutable,
  as long as reclamation keeps chunks a replication slot may still decode
  (above).

### C5. What it keeps from the chunk store as it is

- **Durability and standbys.** Every appended byte is WAL-logged and
  flushed before it's written (`chunkstore_journal.c:31-36, 669-778`), so a
  crash and a physical standby see a value's chunks before any heap commit
  that names it; and a transaction's chunks are written before it becomes
  visible, so another backend never sees a row whose chunks it can't
  read.
- **Rollback.** A transaction's chunks are XCHUNK records, invisible to
  others until it commits; an aborted transaction's chunks are unreachable
  garbage.
- **Branch isolation.** Content-addressed chunks are immutable; the pointers
  live in block-mapped, branch-aware heap pages.

### C6. Coexistence

OID pointers and hash pointers coexist in one table, as oid and oid8 do.
Existing values move when their rows are rewritten; a table rewrite
(`VACUUM FULL`, `CLUSTER`) converts them all. A table keeps its TOAST
relation (decided 2026-10-18): it is needed while the table holds OID
pointers, and dropping it later buys little. B3's memo stays while OID
pointers exist, its size a setting, `prolly_value_memo_entries` (8192
today, fixed).

`pg_upgrade` of a cluster with hash pointers is unsupported (the
chunk store isn't carried over); `pg_dump` writes values detoasted, so it
needs only tests.

### C7. Open questions

1. **jsonb's canonical bytes.** Either its on-disk format (deterministic and
   version-tagged, so C folds it without decoding) or its text, as now
   (portable, so C rehashes jsonb). Recommended: on-disk, with the version
   byte in the canonical encoding. **Decided (user, 2026-10-17): on-disk.**
2. **The hash in the pointer.** A tag per hash algorithm (C1). A profile
   change to another hash adds a tag; old pointers keep theirs and still
   name trees in the old hash. Recording only the hash's length would not
   do: two 32-byte hashes have the same length, and a variable-length
   pointer under one tag would break `VARTAG_SIZE` and `VARSIZE_EXTERNAL`
   for every tuple walker. **Decided (user, 2026-10-17): a tag per
   algorithm.**
3. **Gates before building.** Per-value save cost against heap TOAST at 2
   KB, 64 KB and 1 MB, single and concurrent writers; and reclamation's
   new roots (C4). **Decided (user, 2026-10-18):** a throwaway prototype
   of the save path first, used to measure the gates (save cost at 2 KB,
   64 KB and 1 MB with one writer and with concurrent writers; a
   collection with the working set's roots), then C built for real.
4. **A hybrid.** Hash pointers only above a size, OID TOAST below.
   **Decided (user, 2026-10-18):** no hybrid unless gate 3 fails; then a
   fixed 64 KB, in code (a format rule, not a setting, as `T` is).
   **Superseded (round 2, C8):** gate 3 failed at every size, so a 64 KB
   hybrid wouldn't have passed it either; no hybrid, and the journal's
   append is reworked instead.
5. **The format.** **Decided (user, 2026-10-18):** a clean break of
   history's format is acceptable until spec 11's step 8 (measurement)
   is done, as every format change before it has been; after it, older
   formats stay readable.
6. **With them**, decided as recommended: `T` stays 2048; jsonb's on-disk
   canonical bytes (question 1) come first, as a PR of their own; the
   destination is chosen by a toaster hook keyed on the relation's access
   method, not a copy of heap's routine (C2); a collection's roots
   include the versioned heaps, dead tuples too, and the chunks appended
   after the oldest slot's position (C4); out-of-line values rely on the
   table files' LZ4 (C4); `prolly_value_memo_entries`, `VACUUM FULL` and
   `CLUSTER` converting, the TOAST relation kept, `pg_upgrade`
   unsupported and `pg_dump` tested only (C6).

### C8. Measured gates and round 2 decisions

A throwaway prototype of the save path (kept off master) measured gates
1 and 3, on a release build (`-O2`, no assertions, OpenSSL) with random,
incompressible values. Three setups: a stock heap table; a versioned
table as built (TOAST in its TOAST table, "OID"); and the prototype
("hash"), which built a value tree in `toast_save_datum` and wrote a
SHA-256 pointer.

**Gate 3, save cost.** Against heap TOAST, as question 3 set it; no pass
line was set with it, and the comparisons below give the ratio of heap's
tps to hash's (how many times as much a value costs). pgbench, one INSERT
a transaction, tps (mean of two 10 s runs):

| size | writers | heap | OID | hash | hash, append outside the lock | hash, no flush of its own |
|---|---|---|---|---|---|---|
| 2 KB | 1 | 1306 | 1275 | 656 | 655 | 1241 |
| 2 KB | 8 | 5105 | 5440 | 670 | 2751 | 5544 |
| 64 KB | 1 | 976 | 933 | 523 | 550 | 908 |
| 64 KB | 8 | 4623 | 4340 | 958 | 2337 | 3724 |
| 1 MB | 1 | 210 | 204 | 145 | 146 | 178 |
| 1 MB | 8 | 550 | 556 | 286 | 458 | 567 |

One statement inserting many values, one commit (CPU), µs a value with
the values' generation subtracted: 2 KB 12.0 (heap), 11.9 (OID), 17.8
(hash); 64 KB 161, 166, 303; 1 MB 2839, 2919, 4700.

- With the journal as built, gate 3 fails: 1.9–2.0x heap's cost per value
  with one writer at 2 KB and 64 KB, 4.8x (64 KB) to 7.6x (2 KB) with
  eight.
  `append_bytes()` holds `ProllyJournalLock` across `XLogFlush()` and the
  write, so writers take turns at an fsync, and a transaction flushes
  twice (its chunks, then its commit record).
- A 64 KB hybrid doesn't help: 64 KB and 1 MB values fail too with eight
  writers. What fails is the append, at every size.
- Appending outside the lock removes most of the concurrent loss; not
  flushing the chunks on their own as well (an unsafe stand-in for
  writing them after the commit record's flush, as a root is written over
  its PAD) costs 0.92–1.24x as much as heap (0.98–1.17x the versioned
  table as built): 81–109% of heap's tps.
- Where the CPU goes (64 KB, perf): the value chunker's rolling hash about
  32%, SHA-256 22%, the journal's hash-table lookups 15%, CRC 4%.
- Found on the way: a backend's first chunk-store miss scans the whole
  unsealed journal to index it, about 1 s a GB (3.0 s at 3.2 GB). With C
  the journal grows with the working set's writes, not with commits.

**Gate 1, the heap roots.** Warm buffers, one branch's heap read through
shared buffers (not the page store's blocks from disk, the method decision
3 chose); a table of 1 M rows with 2 KB
values and 100 K of them updated (98 MB, 1.1 M pointers counting dead
tuples), and one of 5 M narrow rows (365 MB, none):

| scan | pages | pointers | ms |
|---|---|---|---|
| documents, deforming each tuple | 12,500 | 1.1 M | 221–239 |
| documents, bytes (conservative) | 12,500 | 1.1 M + 1,118 false | 207 |
| narrow, deforming | 46,729 | 0 | 79 |
| narrow, bytes | 46,729 | 696 false | 38 |

About 0.2 s a GB of heap plus 0.2 µs a pointer (a hash set's insert; a
collector marks by slot instead), against 7–10 s for the collection
itself of a 5 GB journal. Scanning each branch's heap would multiply by
the branches; scanning the page store's physical blocks reads each once.
Gate 1 passes for warm scans; a cold scan of the page store's blocks
reads them from disk at the device's sequential rate on top of this, and
is measured on the layer that builds it. Gate 2 (compression) wasn't
measured.

**Decided (user, 2026-10-18, round 2):**

1. No hybrid. C's first engine layer reworks the journal's append: the
   range is reserved under the lock and logged and written outside it,
   batches share a group flush, a transaction's last batch is written
   after its commit record's flush, and a range left unwritten is padded.
   The last batch is written before the transaction is visible (after
   the commit record's flush, before CLOG and the ProcArray), so C5 still
   holds: a commit's chunks are present before anyone sees a row naming
   them. Gate 3 is measured again on it.
2. The collection's heap roots land before the save hook, so no build
   saves values a collection could free.
3. The roots are found by a conservative byte scan of the page store's
   physical blocks: each block once, whichever branches map it, freed
   blocks skipped. Bytes that spell a hash pointer keep its tree; nothing
   named is lost (as the collector already treats chunks' bytes). Dirty
   buffers are flushed to the page store before the scan, and a pointer
   written after that is covered by the keep set: a transaction that
   names a tree is waited for by a collection that begins later, and one
   that names it while a collection runs adds it to the collection's keep
   set.
4. The journal is bounded: past a size, autovacuum seals it and moves its
   chunks into a table file, collecting nothing.

**Decided (user, 2026-10-18, round 3):**

5. Gate 3 passes at 1.5x: saving a value by content may cost up to one
   and a half times what heap TOAST's does, per value, at each size and
   writer count measured.
6. Logical slots on a standby, which the primary's collections can't see,
   are closed with the WAL scan of #300: the standby feeds its slots'
   oldest position back to the primary, as hot standby feedback carries
   its xmin, and a collection keeps what either may still decode. Until
   then it is an open gap, as #300's other two cases are.
7. The roots' scan of every page-store block each collection (decision 3)
   stays as built; scanning only the blocks changed since the last
   collection is a follow-up, #340.

**The layers**, one stack, each with its tests. Layers 7 to 10 merge
together: from the save hook on, values are saved by content, and only
layer 10 lets logical decoding read them and keeps what a slot still has
to decode.

1. This part.
2. jsonb's on-disk canonical bytes, with a version byte (C7 question 1).
3. The journal's append (decision 1), gate 3 measured again.
4. The journal's bound (decision 4).
5. The pointer (C1): `VARTAG_ONDISK_SHA256`, the fetch hook (whole and by
   slice), and the audit of `VARATT_IS_EXTERNAL_ONDISK`'s uses.
6. The collection's roots (C4, decision 3): hash pointers in the page
   store, and the keep set while a collection runs.
7. The save hook (C2): the toaster's destination by access method.
8. The fold (C3): a hash pointer's hash is the column's reference.
9. Apply (C3): hash pointers written for any row.
10. Logical decoding (C4), and a collection keeping the chunks appended
    after the oldest replication slot's position.
11. `VACUUM FULL` and `CLUSTER` convert (C6).
12. The acceptance tests (#181).

**As built (the layers above).**
- **The journal's append.** An append reserves its range at the
  segment's end and WAL-logs its bytes under `ProllyJournalLock`, and
  writes them outside it: a batch once the log is flushed (a flush
  concurrent appends and commits share), a transaction's last chunks and
  its PAD once its commit record is flushed (reserved by the store's new
  `commit_writes()` at the end of the commit's pre-commit steps, which
  also makes such a commit flush at once), with the ROOT record written
  in the PAD's place, not over it after: a collection's window, which
  waits for the ranges in flight, finds the ROOT, never a PAD that
  becomes one once the window has marked (#328). The ranges reserved
  and not written are listed in shared memory: a reader skips one (met at its
  start or inside it) and reads it once written, a checkpoint and a
  collection wait for those in flight as they begin, an abort writes its
  own. A crash can leave a range the WAL never
  had at a segment's end; the next append after a restart cuts it off
  (truncated, not padded: it is the end). Gate 3 measured again on it
  (the prototype's save path on this append, tps, mean of two 10 s runs):
  2 KB 1276 heap / 1247 OID / 1234 hash with one writer, 4957 / 5045 /
  5355 with eight; 64 KB 1055 / 955 / 925 and 4792 / 4312 / 3734; 1 MB
  212 / 206 / 160 and 611 / 607 / 471: at most 1.33x heap TOAST per
  value. No pass line was set for gate 3 (C8); the user is asked to
  confirm this one passes.
- **The journal's bound.** The reclamation's new `move()` collects the
  journal's sealed segments only (a collection's scope, `gc_begin()`'s:
  default, full or the journal), marking every chunk, into a table file
  that isn't settled, so the next default collection still collects its
  garbage; table files are left as they are. Autovacuum runs it when the
  journal's segments together (sealed ones too) have passed
  `prolly.journal_move_threshold` (256 MB; 0 refused, -1 off), paced by
  its cost-based delay.
- **The pointer.** `VARTAG_ONDISK_SHA256` (5); `va_extinfo`'s high bits
  are the form the tree holds the value in: its bytes, a text-like
  value's in UTF-8 (another encoding), or a jsonb document's canonical
  bytes. A destination (`toast_destination.h`) is found by the pointer's
  tag (fetch) and by the table's access method (save); the chunk store's
  is `toast_chunkstore.c`. The tree interface gets `value_reference()`
  and `value_root()`.
- **The roots.** The page store's allocated blocks, each read once (until
  two reads agree), their items dead or alive, not their free space;
  `keep()` notes a transaction that names a tree (a collection begun
  later waits for it to end or be prepared, as its pointers are on pages
  then; a collection waits for no prepared transaction, keeping what one
  appended to the journal as roots) and adds the tree to a running
  collection's keep set (`gc_kept()`, before the window and in it; too
  many to remember, or no set free, keeps every chunk). It does so under
  the journal's lock, which the window holds from its last `gc_kept()` to
  the swap, and fails (a serialization failure) if a swap dropped the
  tree first. A sweep of the page store waits for the scan. A pointer a
  row is given as it is is told to `keep()` too (the destination's
  `reference()`).
- **Not as designed:** `debug_toast_relation`, a developer option
  (superuser-only), sends values to the TOAST relation all the same, so
  tests can make OID pointers. A value is stored converted (text in
  UTF-8, jsonb's canonical bytes) only if converting back gives its bytes
  back and both are short enough to convert; otherwise, in SQL_ASCII, and
  for every other type, its tree holds its bytes, which a fold reads (a
  pointer doesn't say its type, so a fetch can't decode another type's
  canonical bytes). An apply or merge writes pointers from history for
  bytea and text in UTF-8, keyed tables only; jsonb is decoded (and so
  checked) as it is written back. A short value moved out (its row was long) is read by a fold
  and stored inline, as designed; so is text in another encoding by an
  apply (its size is only known converted back). A logical slot's
  protection is by segment: a collection keeps as they are the sealed
  segments sealed after the oldest slot's `restart_lsn` (the manifest
  records each segment's seal position, version 3). Two cases aren't
  protected, both only when the row naming the value is deleted and
  pruned before the slot decodes it: a value a transaction after the
  slot's position reused (deduplicated, so nothing was appended), and a
  value appended to a segment sealed before the slot's position. Closing
  them, by a collection scanning the WAL after the oldest slot's
  `restart_lsn` for hash pointers, is #300 (decided 2026-10-18: a
  follow-up). A kept segment's roots are its chunks, not an aborted
  transaction's nor its ROOT records. Logical slots on a standby aren't
  seen by the primary's collections either (a third gap, open).
- **The memo's size** is the server setting `prolly_value_memo_entries`
  (core naming, decided 2026-10-18), not `prolly.value_memo_entries`; 0
  turns it off.
- **Measured (acceptance):** a 32-byte slice from the middle of a 100 MB
  bytea reads 4 nodes, whether TOAST compressed it before moving it out
  or not (the test allows at most 4). Stock heap TOAST is unchanged (release builds of master and of
  C, heap inserts of random values, tps, mean of three 10 s runs): 2 KB
  1255 / 1261 with one writer, 5150 / 5198 with eight; 64 KB 1041 / 1034
  and 5004 / 5025.

## 7. Large objects

`pg_largeobject` and `pg_largeobject_metadata` are shared by all branches
and never folded (`branch.c:94-130`). Before #217 an `oid` or `lo` column
was folded as the number only, so reset and merge didn't restore an
object's contents, and `dolt_status` didn't report it as not covered.
Question 35 decided that a commit refuses a column of a type with no
OID-free encoding (`oid` itself); that is built (below). Versioning large
objects later can reuse C: an object's pages as one value tree, and an
`lo` reference carrying its hash.

**As built (#217).** A commit refuses a table or materialized view with a
column whose type holds OIDs: `oid`, `oid8`, the `reg*` types,
`pg_node_tree` (whose text names objects by OID), and the domains
(contrib's `lo`), arrays, composites, ranges and multiranges made of
them. Status and diff list each such column (`schema.table.column`) as
not covered and still fold its table, so its other changes show. A
reset, checkout or checkpoint folds the working set as a commit does, so
it is refused too until such a column is dropped, as for every object a
commit refuses. Types that aren't OIDs but are local (`xid`, `tid`,
`pg_lsn`) are versioned as the numbers they are; `aclitem` has no binary
send function, so its tables can't be folded at all.
- **Not as designed:** the `reg*` types are refused too. Question 35
  decided portable codecs for them (by qualified name); those aren't
  built, and their send functions send the OID, so until they are a
  commit refuses them rather than hashing a local identifier. Unknown
  extension types aren't refused: they are stored as their text output
  (question 35, decided 2026-10-15).

## 8. Order

1. Question 35's refusal of `oid` columns, so large-object references
   aren't silently versioned as numbers. Built (#217).
2. B2, then B1 (B1's references use B2's trees), then B3 and B4. Built
   (#218, #219, #220, #221).
3. C's prototype and gates (C7, question 3; measured, C8), then C as
   C8's layers, jsonb's on-disk canonical bytes (C7, question 1) the
   first (#181).
