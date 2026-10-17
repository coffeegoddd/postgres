# 13. Tables without a primary key: prolly, heap and Dolt

**Status: measured 2026-10-17, on master plus stack #138 (#135–#139,
with their reviews' fixes).**
This compares how stock Postgres (heap), page-based prolly and Dolt store
tables without a primary key ("keyless" tables), what their operations
cost, and how their merges treat them. Where prolly falls short, the
findings at the end say what to change. Nothing here is decided.

## 1. How each one keeps a keyless row

| | Heap | Prolly (pages + history) | Dolt |
|---|---|---|---|
| Working set | Heap pages; a row is a tuple at a TID | Heap pages, the same as heap | A prolly tree, the same as history |
| Row identity in history | None | The row's canonical encoding is its key; the value is the row plus a count of copies | A 16-byte xxh3-128 hash of the row's encoded columns is its key; the value is a count of copies plus the columns |
| Order of the history tree | — | By content, in the columns' order (the keying's compare) | By hash: effectively random |
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
it); the merge cases in part 4 are its `merge_pg.sql` and
`merge_dolt.sql`.

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

**Keyless tables hurt Dolt's history.** Their hashes put rows in random
order, so a range of changed rows touches as many leaf chunks as rows,
where the same range keyed touches a few: keyless, Dolt's journal grew to
27 times the keyed run's, a 10,000-row diff took 6 times as long, and
merges and cherry-picks of 10,000 rows 14 and 10 times as long. Its
merges of keyless tables also never use the subtree-splicing fast path
(`canFastMergeProllyTrees := !keyless && …`,
`libraries/doltcore/merge/merge_prolly_rows.go`).

**Keyless tables hurt prolly's folds, through one gap.** With #136 a
keyless table folds from its changed pages, but a fold made in the
transaction that wrote the rows leaves the table marked unsettled, and
the next fold reads it in full. Every merge, cherry-pick and revert
commits in the transaction that applied its rows, so after each one the
next fold of a keyless table is a full one: in this run the cherry-pick
(4.8 s, of which most is `fold_all`'s sort and tree build) and the
checkouts folded the whole table. The cherry-pick alone, after plain
commits, folds only its pages (counted: 38 pages, no full table).

**Checkouts fold even when nothing changed.** Leaving a branch checks
whether its working set equals its head by folding it; a branch with
nothing changed takes no checkpoint, so its baseline never advances and
every checkout folds every change since the branch's last commit's
baseline again: 35 ms keyless (full folds), 86 ms keyed (lookups of the
carried and changed keys), for one checkout with nothing changed. "Has
anything changed?" should be O(1) a table (`10-complexity-requirements.md`).

**The first commit folds every row** (1.3–1.4 s for a million), once; Dolt
had paid that cost while loading (5.7 s against prolly's 0.18 s).

## 4. Merges of keyless rows

The same six cases in both, each from a committed base, one change on a
branch and one on `main`, then `dolt_merge` into `main`:

| Case (base → ours, theirs) | Prolly | Dolt |
|---|---|---|
| Both add a copy of an existing row (1 → 2, 2) | Merges: 2 copies | Conflict (base 1, ours 2, theirs 2) |
| Both add the same new row | Merges: 1 copy | Conflict |
| Each adds a different row | Merges: both | Merges: both |
| Both delete the same row | Merges: deleted | Conflict |
| Both update the same row differently | Merges: both new rows, old one gone | Conflict on the shared delete; both new rows in the working set |
| Base has 2 copies; ours deletes one, theirs adds one | Merges: 2 copies | Conflict |

Prolly's rule (`merge_table`, `diffmerge_folded.c`): changes in a row's
copies add up, unless both sides made it the same number, which converges
as a keyed row's identical change does; a conflict only when theirs removes
more copies than ours has. Dolt's: any key both sides changed is a
conflict, identical changes too (`valueMerger.TryMerge` returns at once for
keyless; `merge_rows.go:424`, "For keyless tables, this counts as a
conflict"). Dolt's own tests skip the update case as resolving incorrectly
(`integration-tests/bats/keyless.bats:685`).

Prolly's convergence rule gives 2 copies, not 3, when both sides add a copy
of the same row: two people each adding a row is read as both making the
same change. That is the keyed rule carried over, and a choice to confirm,
not an accident; summing would give 3.

## 5. What to change in prolly

In order of what the numbers say matters:

1. **A fold in the writing transaction shouldn't mark a keyless table
   unsettled.** Its own writes are settled once it commits, and its Dolt
   commit rolls back with it if it doesn't. A second fold in the same
   transaction has to tell its own earlier writes apart by command id,
   which the baseline state doesn't record yet. This removes the full fold
   after every merge, cherry-pick and revert.
2. **Checkouts shouldn't fold to see whether a branch is clean.** Each
   table's changed-page list and its baseline say whether anything changed
   since the head, in O(1) a table; fold only when something did, and then
   advance the baseline.
3. **Decide the keyless merge rule:** keep prolly's (counts add up,
   identical changes converge), take Dolt's (any change on both sides
   conflicts), or sum always (both adding a copy gives 3).
4. **Moves are O(rows changed), Dolt's O(1).** That follows from keeping
   the working set in pages; worth measuring again after 1 and 2, since a
   fast-forward here also paid for folds, and a dirty checkout is a fold.
5. **The journal grows with every full fold** (1.1 G here, keyless): likely
   the trees rebuilt by full folds written again, unverified; step 7's GC
   reclaims them, and 1 removes most of them.
