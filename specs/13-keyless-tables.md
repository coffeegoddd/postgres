# 13. Tables without a primary key: prolly, heap and Dolt

**Status: measured 2026-10-17, on master plus stack #138 (#135–#139,
with their reviews' fixes); prolly again in part 6, on `e25140de87` (#141
and #142 before their reviews), and in part 7 with keyless rows keyed by
their first bytes (#149) and merged by the rule #150 makes, on
`a2c14b4b68`. The keyed table's space was broken down by component in
part 8, for #500, under the `log` change finder.**
This compares how stock Postgres (heap), page-based prolly and Dolt store
tables without a primary key ("keyless" tables), what their operations
cost, and how their merges treat them. Where prolly falls short, the
findings at the end say what to change; part 5 says which of them the
user decided, and parts 6 and 7 measure them.

## 1. How each one keeps a keyless row

| | Heap | Prolly (pages + history) | Dolt |
|---|---|---|---|
| Working set | Heap pages; a row is a tuple at a TID | Heap pages, the same as heap | A prolly tree, the same as history |
| Row identity in history | None | Since #149: the first 32 bytes of the row's encoded values, then its SHA-256 hash, are its key (before: the hash alone); the value is a varint count of copies, then the row | A 16-byte xxh3-128 hash of the row's encoded columns is its key; the value is a count of copies, then the columns |
| Order of the history tree | — | Since #149: by the rows' first bytes, so rows that begin alike are together (before: by hash, effectively random) | By hash: effectively random |
| Duplicate rows | One tuple each | One tree entry, its count | One tree entry, its count |
| UPDATE | A new tuple version | A new tuple version; in history, the old content's count drops and the new content's rises | Delete of the old hash, insert of the new: two tree edits per row, at unrelated places in the tree |
| Secondary index | Points at TIDs | Points at TIDs (they are heap indexes) | Indexed columns plus the row's hash; every lookup goes back to the clustered tree for the count |

Dolt's code, for each claim: `store/val/keyless_tuple.go`
(`HashTupleFromValue`: the hash excludes the count),
`libraries/doltcore/sqle/writer/prolly_index_writer_keyless.go` (writes:
one lookup plus one put per row, the count adjusted),
`libraries/doltcore/sqle/index/index_reader.go:66` (keyless indexes always
look the row up again for its count). Dolt version 2.4 (binary) and 2.4.2
(source, `51cc4458b7`).

## 2. What was measured

One table `t (a int, b varchar(32), c int)`, 1,000,000 rows, an index on
`a`; the last 100,000 rows repeat the first 100,000 exactly, so 100,000
rows have two copies. The same table with a primary key (`id bigserial`,
or `AUTO_INCREMENT` in Dolt) is the control. Each engine ran alone, a fresh
server each time, on a 16-core machine with its data on tmpfs, so fsyncs
cost nothing: the numbers are CPU and design costs, not durability costs.
Release builds: stock Postgres at upstream `a999971331` (the commit this
fork last synced), prolly at the top of stack #138, Dolt 2.4.0. Each step
is one statement or call through one connection (Go `database/sql`,
pgx and go-sql-driver/mysql); the median of its repetitions is shown (the
mean of the middle two for an even count). The
harness is `src/tools/prolly_bench/keyless/` (its README says how to run
it); the merge cases in part 4 are the `merge_pg.sql` and
`merge_dolt.sql` its `gen.py` writes into the work directory.

### Writes and reads (ms)

| Operation | Heap | Prolly keyless | Dolt keyless | Prolly keyed | Dolt keyed |
|---|---|---|---|---|---|
| Load 1M rows (COPY / LOAD DATA) | 168 | 183 | 5,749 | 701 | 3,258 |
| Create the index on `a` | 133 | 136 | 742 | 132 | 572 |
| Update one row (by `a`) | 0.10 | 0.09 | 0.60 | 0.06 | 0.39 |
| Update a row with two copies | 0.07 | 0.07 | 0.50 | 0.06 | 0.44 |
| Insert one row | 0.07 | 0.04 | 0.41 | 0.05 | 0.40 |
| Delete one row | 0.05 | 0.06 | 0.57 | 0.05 | 0.54 |
| Update 10,000 rows (range of `a`) | 15 | 16 | 1,313 | 24 | 1,075 |
| Delete 10,000 rows | 2.2 | 2.2 | 1,160 | 2.2 | 1,006 |
| Insert 10,000 rows (INSERT … SELECT) | 7.3 | 7.4 | 210 | 13 | 50 |
| `count(*), sum(c)` over every row | 16 | 17 | 65 | 17 | 68 |
| `DISTINCT` over every row | 130 | 132 | 262 | 156 | 268 |

Prolly's working set is heap's pages, and it writes and reads as heap
does, keyless or not. Dolt writes into its tree at once, so each row costs
a tree lookup and put (and, keyless, a second one for an update). Against
heap, keyless: 6–11 times for a one-row write, 29 times for inserting
10,000 rows, 34 for the load, 85 for updating 10,000 and about 520 for
deleting 10,000.

### Version control (ms)

| Operation | Prolly keyless | Dolt keyless | Prolly keyed | Dolt keyed |
|---|---|---|---|---|
| First commit of the 1M rows | 1,446 | 1.1 | 1,320 | 1.2 |
| Commit after one row changed | 0.45 | 0.27 | 0.61 | 0.34 |
| `dolt_status` after one row | 0.56 | 0.16 | 0.37 | 0.17 |
| `dolt_diff` HEAD–WORKING after one row | 0.47 | 0.27 | 0.39 | 0.24 |
| Commit after 10,000 rows updated | 217 | 0.95 | 36 | 0.69 |
| `dolt_diff` of two commits 10,000 rows apart | 55 | 41 | 2.8 | 7.3 |
| `dolt_status` after 10,000 rows updated | 211 | 0.62 | 68 | 0.62 |
| Hard reset of 10,000 changed rows | 297 | 0.39 | 120 | 0.54 |
| Make a branch | 0.19 | 0.28 | 0.18 | 0.34 |
| Check out a new branch (its first use) | 78 | 0.12 | 183 | 0.13 |
| Check out another branch, nothing changed | 35 | 0.09 | 86 | 0.12 |
| Check out another branch, 10,000 rows changed | 404 | 0.54 | 130 | 0.58 |
| Fast-forward merge of 10,000 changed rows | 404 | 0.55 | 108 | 0.38 |
| Three-way merge, 10,000 rows changed on each side | 316 | 194 | 76 | 13 |
| Three-way merge, one row changed on each side | 1.7 | 0.84 | 1.7 | 0.86 |
| Cherry-pick of 10,000 changed rows | 4,850 | 143 | 181 | 14 |
| `count(*)` of `dolt_log` | 0.28 | 0.29 | 0.30 | 0.41 |

Each row is one statement or call, repeated: the 10,000-row status and
the dirty checkout reset the branch before each repetition, so each saw
10,000 changed rows; a checkout is one switch, the way back untimed.

### Space after the run (bytes)

| | Heap | Prolly keyless | Dolt keyless | Prolly keyed | Dolt keyed |
|---|---|---|---|---|---|
| Database on disk | 94 M | 1,303 M | 1,248 M | 248 M | 46 M |

Neither history store had been garbage-collected (prolly has no GC yet,
step 7; Dolt's `dolt gc` wasn't run). In the first run of this comparison
prolly's keyless total was 1,132 M of chunk journal and 143 M of pages.

## 3. What the numbers say

**Dolt pays for history at write time; prolly at commit and move time.**
Dolt's working set is its tree, so a commit, a status, a branch switch, a
reset or a fast-forward only names a root: under a millisecond whatever
changed. Prolly writes as heap does and folds the changes into a tree when
it commits: updating 10,000 rows and committing them costs prolly 24 + 36 =
60 ms keyed, Dolt 1,075 + 1 = 1,076 ms. Moving a branch's pages to another
commit (a reset, a fast-forward) applies the difference as rows, about
11–12 µs a row keyed; Dolt only moves a pointer.

**A dirty checkout costs prolly a fold, not a move.** In both, a checkout
leaves the uncommitted changes on the branch left (each branch has its
own working set: checked in Dolt's SQL session, where the other branch
doesn't see them). Dolt only switches roots; prolly checkpoints the
branch left, folding its changes (130 ms keyed for 10,000 rows, 404 ms
keyless, which the next finding inflates).

**Keyless tables hurt both histories** (prolly's, as part 2 measured it,
before #149; part 7). In both, their hashes put rows in random order
(prolly's keying, `keying_pkcontent.c`, keyed a row by its content's
hash, as Dolt does: a first draft of this said otherwise), so
a range of changed rows touches as many leaf chunks as rows, where the
same range keyed touches a few. Keyless, Dolt's journal grew to 27 times
the keyed run's, a 10,000-row diff took 6 times as long, and merges and
cherry-picks of 10,000 rows 14 and 10 times as long. Dolt's merges of
keyless tables also never use its subtree-splicing fast path
(`canFastMergeProllyTrees := !keyless && …`,
`libraries/doltcore/merge/merge_prolly_rows.go`). Prolly's journal showed
the same: 21 times the keyed run's (part 6's last rows); since #149, 1.6
times (part 7).

**Keyless tables hurt prolly's folds, through one gap** (as part 2 was
measured; #141 closes it, part 6). With #136 a keyless table folds from
its changed pages, but a fold made in the transaction that wrote the
rows left the table marked unsettled, and the next fold read it in full.
Every merge, cherry-pick and revert commits in the transaction that
applied its rows, so after each one the next fold of a keyless table is
a full one: in this run the cherry-pick (4.8 s, of which most is
`fold_all`'s sort and tree build) and the checkouts folded the whole
table. The cherry-pick alone, after plain commits, folds only its pages
(counted: 38 pages, no full table).

**Checkouts folded even when nothing changed** (as part 2 was measured;
#142 closes it, part 6). Leaving a branch checks whether its working set
equals its head by folding it; a branch with nothing changed takes no
checkpoint, so its baseline never advanced and every checkout folded
every change since the branch's last commit's baseline again: 35 ms
keyless (full folds), 86 ms keyed (lookups of the carried and changed
keys), for one checkout with nothing changed. "Has anything changed?"
should be O(1) a table (`10-complexity-requirements.md`).

**The first commit folds every row** (1.3–1.4 s for a million), once; Dolt
had paid that cost while loading (5.7 s against prolly's 0.18 s).

## 4. Merges of keyless rows

The same six cases in both, each from a committed base, one change on a
branch and one on `main`, then `dolt_merge` into `main`:

| Case (base → ours, theirs) | Prolly, as measured | Prolly, since #150 (from its rule, checked in `merge.sql`) | Dolt |
|---|---|---|---|
| Both add a copy of an existing row (1 → 2, 2) | 2 copies | 2 copies | Conflict (base 1, ours 2, theirs 2) |
| Both add the same new row | 1 copy | 1 copy | Conflict |
| Each adds a different row | Both | Both | Both |
| Both delete the same row | Deleted | Deleted | Conflict |
| Both update the same row differently | Both new rows, old one gone | The same | Conflict on the shared delete; both new rows in the working set |
| Base has 2 copies; ours deletes one, theirs adds one | 2 copies | 2 copies | Conflict |
| Base has 2 copies; ours deletes them both, theirs one | Conflict | Deleted | Conflict |
| Both update the same row alike | The new row once | The same | Conflict |

Prolly's rule as measured (`merge_table`, `diffmerge_folded.c`): changes
in a row's copies add up, unless both sides made it the same number,
which converges as a keyed row's identical change does; a conflict only
when theirs removes more copies than ours has.  Since #150: a row both
sides changed by the same number of copies is changed once; otherwise it
has the base's copies, those each side added, less the most either
removed; it never conflicts. Dolt's: any key both sides changed is a
conflict, identical changes too (`valueMerger.TryMerge` returns at once for
keyless; `merge_rows.go:424`, "For keyless tables, this counts as a
conflict"). Dolt's own tests skip the update case as resolving incorrectly
(`integration-tests/bats/keyless.bats:685`).

Converging identical changes gives 2 copies, not 3, when both sides add
a copy of the same row: two people each adding a row is read as both
making the same change. #150 first summed them, as the two rows they
might be, but that made every identical change twice: the same update on
both sides turned one row into two, and a hotfix picked onto a branch
and then merged from its own came twice (the user chose convergence, for
those ordinary cases over two branches inserting byte-identical rows).
What #150 keeps: removals on both sides count once, and a keyless row
never conflicts. Two different updates of the same row still keep both
new rows: a keyless row has no identity to tell an update from a delete
and an insert.

## 5. What to change in prolly

In order of what the numbers say matters:

1. **A fold in the writing transaction shouldn't mark a keyless table
   unsettled.** Done in #141: its own writes are settled for it (they go
   with it if it rolls back, and what it writes after changes pages
   since), so no command id is needed after all; a baseline records the
   fold's transaction, which a later fold must see committed.
2. **Checkouts shouldn't fold to see whether a branch is clean.** Done
   differently, in #142: what a clean checkout's fold read was what moves
   (a reset, a fast-forward) left changed since a baseline they didn't
   advance, and what #141 now settles. A move folds from the head once, as
   a branch's first use does, and a clean checkout reads nothing (0 pages,
   counted, in `clean_checkouts`): 0.45 ms keyless, 0.60 ms keyed.
3. **Decide the keyless merge rule.** Decided (user, 2026-10-17) and done
   in #150: the same change on both sides is made once; otherwise copies
   added on either side add up and copies removed on both are removed
   once; a keyless row never conflicts (part 4).
4. **Key keyless rows by their content, not its hash.** Decided (user,
   2026-10-17) and done in #149, as proposed below; part 7 measures it.
   Changes to a range of rows would then touch a few chunks, as a keyed
   table's do, not one each: the history grew 21 times as fast keyless as
   keyed (part 6's journal sizes), and a fold of 10,000 changed rows cost
   4–5 times a keyed one; since, 1.6 times and less than a keyed one's
   (part 7). The proposal: a key of the row's
   first bytes as the canonical encoding writes them (32 here), then its
   hash. That orders by bytes, not by any type's comparison, so every
   column type has it (json, xml or a point as well as an int), and a
   wide or TOASTed row's key is no longer than any other's; the hash
   keeps it the row's alone. Rows whose first bytes are random (a UUID
   first) gain nothing, and lose nothing. The canonical format is
   frozen, so this needed a new keying: a clean break, the format
   recorded in each folded root (as built), decided before step 6, since
   remotes carry the format.
5. **Moves are O(rows changed), Dolt's O(1).** That follows from keeping
   the working set in pages: with 1, 2 and 4, a fast-forward of 10,000
   rows is 68 ms keyed (50 keyless), a reset 96 (65); Dolt's are under a
   millisecond (part 2).
6. **The journal doesn't rewrite chunks it has** (`journal_put` skips
   one already held): its growth is new chunks, which 4's scattered
   keyless edits were most of; since 4, every commit's changed chunks
   (86 M keyless, 53 M keyed after the run, part 7), the old versions of
   which step 7's GC reclaims.

## 6. After #141 and #142

Prolly again, the same runs on a release build of `e25140de87` (#141 and
#142 before their reviews; heap and Dolt: see part 2). Times in ms:

| Operation | Prolly keyless (part 2) | Now | Prolly keyed (part 2) | Now |
|---|---|---|---|---|
| Commit after 10,000 rows updated | 217 | 188 | 36 | 40 |
| `dolt_diff` of two commits 10,000 rows apart | 55 | 45 | 2.8 | 2.9 |
| `dolt_status` after 10,000 rows updated | 211 | 185 | 68 | 36 |
| Hard reset of 10,000 changed rows | 297 | 270 | 120 | 102 |
| Check out a new branch (its first use) | 78 | 3.5 | 183 | 4.7 |
| Check out another branch, nothing changed | 35 | 0.45 | 86 | 0.60 |
| Check out another branch, 10,000 rows changed | 404 | 345 | 130 | 63 |
| Fast-forward merge of 10,000 changed rows | 404 | 260 | 108 | 68 |
| Three-way merge, 10,000 rows changed on each side | 316 | 307 | 76 | 71 |
| Three-way merge, one row changed on each side | 1.7 | 1.1 | 1.7 | 1.6 |
| Cherry-pick of 10,000 changed rows | 4,850 | 394 | 181 | 102 |

Space after the run, in bytes (part 2 gave only the totals):

| | Prolly keyless (part 2) | Now | Prolly keyed (part 2) | Now |
|---|---|---|---|---|
| Database on disk | 1,303 M | 1,269 M | 248 M | 245 M |
| of which the chunk journal | 1,132 M | 1,129 M | — | 53 M |

The pages are much the same either way (about 140 M of the totals); the
journal, the history, is 21 times as large keyless (1,128,903,541 bytes
against 52,960,508).

The full folds after merges and picks and the clean checkouts' folds are
gone. What a keyless table still pays over a keyed one has two parts:
its history's order (part 5, item 4) makes its folds and its history
cost more (a commit of 10,000 rows 188 ms against 40, the journal 21
times), and its moves cost 3–5 times a keyed table's (a dirty checkout
345 ms against 63, a fast-forward 260 against 68, a reset 270 against
102, a three-way merge 307 against 71). The second isn't the history's
order alone: a move finds each keyless row it changes by its whole
content (the row applier's index scan, or the keyless index it builds),
where a keyed one is found by its key. Which part costs what isn't
measured yet.

## 7. Keyed by their first bytes

Prolly again, the same runs on a release build of `a2c14b4b68`: #149's
keying (a keyless row's first 32 encoded bytes, then its hash) and #150's
merge rule, over #141 and #142 with their reviews' fixes. Part 6 was
measured before those fixes, so the change from it is theirs and #149's
together, not #149's alone; the keyed column, which #149 doesn't touch,
moved little (a reset 102 to 96 ms), so most of the keyless change is
#149's. (#149's review
since moved the prefix to start after the row's column count and NULLs,
so rows cluster whatever their NULLs; this table's rows have none, so
the keys group as measured here. The format is recorded in each folded
root, "!:format", and a root keyed as before has its tables' rows
unreadable.) Times in ms:

| Operation | Prolly keyless (part 6) | Now | Prolly keyed (part 6) | Now |
|---|---|---|---|---|
| Commit after 10,000 rows updated | 188 | 22 | 40 | 37 |
| `dolt_diff` of two commits 10,000 rows apart | 45 | 3.4 | 2.9 | 2.8 |
| `dolt_status` after 10,000 rows updated | 185 | 22 | 36 | 34 |
| Hard reset of 10,000 changed rows | 270 | 65 | 102 | 96 |
| Check out a new branch (its first use) | 3.5 | 3.8 | 4.7 | 4.8 |
| Check out another branch, nothing changed | 0.45 | 0.45 | 0.60 | 0.64 |
| Check out another branch, 10,000 rows changed | 345 | 45 | 63 | 63 |
| Fast-forward merge of 10,000 changed rows | 260 | 50 | 68 | 68 |
| Three-way merge, 10,000 rows changed on each side | 307 | 53 | 71 | 71 |
| Three-way merge, one row changed on each side | 1.1 | 1.2 | 1.6 | 1.8 |
| Cherry-pick of 10,000 changed rows | 394 | 67 | 102 | 104 |

Space after the run, in bytes:

| | Prolly keyless (part 6) | Now | Prolly keyed (part 6) | Now |
|---|---|---|---|---|
| Database on disk | 1,269 M | 226 M | 245 M | 245 M |
| of which the chunk journal | 1,129 M | 86 M | 53 M | 53 M |

(the journals 86,301,774 and 52,959,296 bytes)

Keyed by their first bytes, keyless rows changed in a run write a few
chunks, and the gaps part 6 found close with it: a keyless table's
commits, statuses and moves now cost what a keyed one's do or less (why
less isn't measured), its diffs about a fifth more (3.4 ms against 2.8),
and its history is 1.6 times a keyed one's, not 21 times. Part 6's guess
that keyless moves cost more for finding rows by content was wrong as
far as these runs show: what they paid was mostly the history's order
too (each move reads and writes the trees), with #141's and #142's
review fixes in the same change. Writes and reads are as before, heap's
(part 2). Dolt's keyless history still has the random order (part 3).

## 8. What the space is (#500)

Parts 2, 6 and 7 gave only totals: a keyed table's database at about
2.5 times heap's (245 M against 94 M), of which 53 M was the journal, and
"the pages much the same either way (about 140 M)". That figure was
taken under the `pages` change finder, with no collection, and with no
way to say what the rest was. `prolly_storage_breakdown()` now says it.
These are sizes only, on an -O2 build of the `stats/storage-breakdown`
stack, with the `log` change finder (the default). The machine was
loaded by other work, so no times were taken. MB here are 10^6 bytes.

**The keyed script again.** Heap keyed and prolly keyed, as in part 2:

| | Heap keyed | Prolly keyed |
|---|---:|---:|
| Database on disk (`pg_database_size`) | 111.0 | 308.4 |
| The table with its indexes, on `main` | 102.6 | 126.1 |
| Page store, every block of it mapped | — | 257.6 |
| — of it, the change logs | — | 92.2, of which 86.4 dead entries and none live |
| — of it, the rest (other branches' copies of pages) | — | about 39 |
| Journal | — | 30.4 |
| Block map (maps, changes lists, allocator, counts) | — | 9.1 |
| VM and FSM | — | 1.9 |
| The chunk store's other files | — | 0.6 |
| Catalogs and the rest | 8.4 | 8.8 |

So against heap's 111 M, prolly's 308 M is 2.8 times. The 140 M of
"pages" in part 6 was never the table's pages costing more: a prolly
table's pages are heap's. It was the table plus its change log, and the
pages the run's branches copied.

The run's change logs are 92 M, 30% of the database. Their entries were
consumed by the run's commits and wait for a vacuum. That is three times
the history's journal (30 M; part 7 measured 53 M).

**Stage by stage (V8, `configs/v_storage.toml`).** A 1M-row table
`(id bigint PRIMARY KEY, payload text)`, beside a heap table given the
same rows and updates, read after each stage:

1. **load:** `INSERT` of the rows;
2. **commit:** the first `dolt_commit`;
3. **updates:** 10 commits, each updating 1% of the rows;
4. **gc:** `dolt_gc()` and `prolly_page_sweep()`;
5. **vacuum:** `VACUUM` of both tables, and another sweep.

| MB | load | commit | updates | gc | vacuum |
|---|---:|---:|---:|---:|---:|
| heap table, `pg_total_relation_size` | 99.1 | 99.1 | 109.0 | 109.0 | 109.0 |
| prolly table, `pg_total_relation_size` | 99.1 | 99.1 | 109.0 | 109.0 | 109.0 |
| page store (`page_store_bytes`) | 167.3 | 167.3 | 184.0 | 184.0 | 184.0 |
| — mapped under a relation's length | 167.3 | 167.3 | 184.0 | 184.0 | 109.0 |
| — held otherwise (`page_store_listed_bytes`) | 0.0 | 0.0 | 0.0 | 0.0 | 75.1 |
| — the table's main fork, as mapped | 76.6 | 76.6 | 84.2 | 84.2 | 84.2 |
| — its index's | 22.5 | 22.5 | 24.7 | 24.7 | 24.7 |
| — its change log (`change_log_bytes`) | 68.3 | 68.3 | 75.1 | 75.1 | 0.0 |
| — — live entries | 64.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| — — dead entries | 0.0 | 64.0 | 70.4 | 70.4 | 0.0 |
| block map | 0.4 | 0.4 | 0.5 | 0.5 | 0.5 |
| VM and FSM | 0.1 | 0.1 | 0.1 | 0.1 | 0.1 |
| journal | 0.0 | 46.2 | 51.0 | 0.0 | 0.0 |
| table files, settled | 0.0 | 0.0 | 0.0 | 51.0 | 51.0 |
| table files, unsettled | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| chunk store's other files | 0.0 | 0.6 | 0.7 | 0.0 | 0.0 |
| **prolly's files** | 167.8 | 214.7 | 236.3 | 235.6 | 235.6 |
| **over the heap table** | 1.69 | 2.17 | 2.17 | 2.16 | 2.16 |

("Prolly's files" are the breakdown's columns that are files. The
catalogs and the heap table are left out.)

What the stages show:

- **The table is heap's, byte for byte.** It is 99,098,624 bytes after
  the load, as the heap table is, and the two grow alike.
- **The change log is the largest overhead.** It is 64 bytes an entry,
  64 MB for a million rows: 65% of the table and its index. A commit
  consumes the entries, and they then stay as dead tuples until a vacuum.
- **A vacuum doesn't return the log's space.** `VACUUM` removes the dead
  entries and truncates the log to 16 kB. But a truncation only shortens
  a block-mapped relation's logical length: its 75.1 MB of blocks stay
  held past that length, for the log to reuse. The page store, and the
  database, don't shrink, and a sweep frees none of it.
- **The history is half the table.** The first commit's tree is 46.2 MB.
  Ten commits of 1% updates add 4.7 MB. A collection keeps all of it,
  since every commit is reachable: it moves the journal into one settled
  table file of 51.0 MB.
- **No copy-on-write.** Under `log`, no page was copied: the page store's
  blocks are all mapped until the vacuum, and none are held by a changes
  list.

Without the change log's 75.1 MB, prolly's files after the gc stage
would be 160.5 MB: 1.47 times the heap table. What to reduce, and the
target, are #500's last step and aren't decided here.
