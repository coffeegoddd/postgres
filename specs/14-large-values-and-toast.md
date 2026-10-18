# 14. Large values: keeping TOAST, versioned

**Status: proposed 2026-10-17, on `64c9001f9a`; revised from review the same day. Decided by the user the same day, as recommended: the `oid` refusal and B are built before step 8's measurements, C after them (part 8), with C7's questions 1 and 2 decided; questions 3 (C's gates) and 4 (the hybrid) stay open until those gates are measured. Nothing here is built yet.**
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
| What decides | Per row, per column: the toaster runs when a row is over 2032 bytes and works down to `toast_tuple_target` | Per row, per column: a row over `target_row_size` (2048) moves columns out | Per row, the whole row: an encoded row over `VALUE_INLINE_MAX` (2048) goes out of line |
| Which column first | The largest eligible (`toast_tuple_find_biggest_attribute`), in four passes by storage strategy | The one saving the most bytes | — |
| Compression | pglz or lz4, per column (`attcompression`), before moving out | None | None: canonical bytes are uncompressed (question 48) |
| Out-of-line form | Rows of `pg_toast_N (chunk_id, chunk_seq, chunk_data)`, 1996-byte chunks, value named by an OID | A blob tree, fixed 4000-byte leaves, named by its hash | A value tree, buzhash chunks of 512–4096 bytes keyed by byte offset, named by its hash (`tree_cow.c:560-608`) |
| Sharing | None: one owner per value; an unchanged value keeps its pointer on UPDATE | Identical values are one tree | Identical rows are one tree |
| Slice reads | Uncompressed values fetch only the chunks they need; pglz fetches a prefix | None | None |
| Deletion | With its row; VACUUM of the TOAST table reclaims | `dolt gc` | No chunk-store reclamation yet |

The versioned working set is heap pages with stock TOAST: a versioned
table's TOAST table and its index are block-mapped and branch-aware
(`heapam_handler.c:489-506`, `versioned.c:164-169`), and branches keep the
same TOAST relation OID with copy-on-write storage, so a pointer resolves
on the session's branch (part 11, lines 78-90). Folding a row into history
decodes every column: `CODEC_TEXT` through `DatumGetTextPP`, other types
through their send functions (`prolly_datum.c:176-191, 594-618`); jsonb's
send form is its text. No TOAST pointer or value OID reaches history, and
TOAST relations are never folded themselves (`canon_catalogs.c:904-906`).

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
   through the applier (`applier_replication.c:575-642`) as decoded datums,
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

### B2. Value trees addressed by byte counts

Value-tree leaves hold chunk bytes with no key; internal nodes record
each child's byte count, as Dolt's blob trees do. Chunk boundaries stay
buzhash. An insertion then rewrites only the chunks around it and the path
above them, and a slice is found by descending through byte counts (C uses
this for `substr`). Existing tests to extend: `large_values` (a one-byte
insertion in the middle writes O(depth) nodes) and
`016_d1_large_values.pl`'s cases moved to versioned tables.

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

### B results

- Invariants: all hold; history never sees TOAST.
- Fixes cost 1 and 2 fully, 3 for unchanged values, 4 for rows whose large
  value didn't change.
- Leaves: a new large value is hashed once at commit; large values are
  stored twice (compressed in TOAST, uncompressed in chunks); historical
  reads have no slice reads; merge still re-toasts a value that moves
  between rows.

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

Where the callback lives is open. Returning a copy of
`GetHeapamTableAmRoutine()` with it set breaks the code that recognizes
heap by its routine's address: `heap_getnext` (`heapam.c:1451`, "only heap
AM is supported") and `REPACK (CONCURRENTLY)` (`repack.c:995`), both of
which prolly tables (and their TOAST tables) pass today. Either those two
checks learn to accept the prolly routine (they check the routine, not the
OID, precisely so that AMs reusing heap's handler work), or the
destination is chosen outside `TableAmRoutine`, by a toaster hook that
looks at the relation's access method, as block-mapped storage is chosen.
Recommended: the hook, which keeps prolly's routine heap's own.

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
  does nothing for hash pointers. The chunk store has no reclamation today
  (`chunkstore_journal.c` conjoins table files; `prolly_gc()` is the D1 node
  store's, `reclaim_marksweep.c`). C needs one, marking from every commit
  root, every branch's working-set heap (scanning versioned heaps for hash
  pointers), and every chunk newer than the collection's start. Working-set
  churn (a 1 MB document updated a thousand times between dolt commits) is
  garbage until then. Dolt has the same property. Two things the roots
  alone miss:
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
    commits) needs a horizon of its own, part 8 of spec 11.
- **Compression moves below the hash.** Values out of line lose TOAST's
  compression unless the chunk store compresses chunks (question 48 left
  "compressing pages" as a later storage choice). Compressing chunks keeps
  hashes over uncompressed bytes.
- **Write throughput.** Chunk-store appends serialize on one
  `ProllyJournalLock` and flush WAL per batch (`chunkstore_journal.c:470-525`).
  A TOAST-heavy insert load goes through it.
- **Logical decoding.** The reorder buffer reassembles TOAST values from
  the TOAST table's WAL. Hash pointers have no TOAST table rows; decoding
  fetches from the chunk store, which is safe because chunks are immutable,
  as long as reclamation keeps chunks a replication slot may still decode.

### C5. What it keeps from the chunk store as it is

- **Durability and standbys.** Every appended byte is WAL-logged and
  flushed before it's written (`chunkstore_journal.c:18-25, 488-525`), so a
  crash and a physical standby see a value's chunks before any heap commit
  that names it.
- **Rollback.** A transaction's chunks are XCHUNK records, invisible to
  others until it commits; an aborted transaction's chunks are unreachable
  garbage.
- **Branch isolation.** Content-addressed chunks are immutable; the pointers
  live in block-mapped, branch-aware heap pages.

### C6. Coexistence

OID pointers and hash pointers coexist in one table, as oid and oid8 do.
Existing values move when their rows are rewritten; a table rewrite
(`VACUUM FULL`, `CLUSTER`) converts them all. A table's TOAST relation is
kept while it holds OID pointers.

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
   KB, 64 KB and 1 MB, single and concurrent writers; and a reclamation
   design for the chunk store.
4. **A hybrid.** Hash pointers only above a size (64 KB?), OID TOAST below.
   Only if gate 3 shows small values are too costly in the chunk store.

## 7. Large objects

`pg_largeobject` and `pg_largeobject_metadata` are shared by all branches
and never folded (`branch.c:94-130`); an `oid` or `lo` column is folded as
the number only, so reset and merge don't restore an object's contents, and
`dolt_status` doesn't report it as not covered. Question 35 decided that
a commit refuses a column of a type with no OID-free encoding (`oid`
itself); that isn't built, and should come before B. Versioning them
later can reuse C: an object's pages as one value tree, and an `lo`
reference carrying its hash.

## 8. Order

1. Question 35's refusal of `oid` columns, so large-object references
   aren't silently versioned as numbers.
2. B2, then B1 (B1's references use B2's trees), then B3 and B4.
3. C's gates (part 6, question 3), then C.
