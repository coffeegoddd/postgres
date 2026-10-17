# 13. Tables without a primary key: prolly, heap and Dolt

**Status: measured 2026-10-17, on master plus stack #138 (#135–#139).**
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
pgx and go-sql-driver/mysql); the median of its repetitions is shown. The
harness and scripts were in the session's scratch space, not committed.

### Writes and reads (ms)

| Operation | Heap | Prolly keyless | Dolt keyless | Prolly keyed | Dolt keyed |
|---|---|---|---|---|---|
| Load 1M rows (COPY / LOAD DATA) | 173 | 171 | 6,215 | 713 | 3,601 |
| Create the index on `a` | 134 | 142 | 830 | 133 | 595 |
| Update one row (by `a`) | 0.06 | 0.09 | 0.65 | 0.07 | 0.38 |
| Update a row with two copies | 0.07 | 0.07 | 0.55 | 0.07 | 0.44 |
| Insert one row | 0.04 | 0.04 | 0.41 | 0.04 | 0.46 |
| Delete one row | 0.05 | 0.05 | 0.44 | 0.05 | 0.61 |
| Update 10,000 rows (range of `a`) | 16 | 16 | 1,405 | 25 | 1,155 |
| Delete 10,000 rows | 2.2 | 2.1 | 1,215 | 2.1 | 1,085 |
| Insert 10,000 rows (INSERT … SELECT) | 7.0 | 7.4 | 215 | 13 | 58 |
| `count(*), sum(c)` over every row | 15 | 16 | 68 | 16 | 73 |
| `DISTINCT` over every row | 132 | 131 | 264 | 164 | 283 |

Prolly's working set is heap's pages, and it writes and reads as heap
does, keyless or not. Dolt writes into its tree at once, so each row costs
a tree lookup and put (and, keyless, a second one for an update): 50–500
times heap's cost for writes.

### Version control (ms)

| Operation | Prolly keyless | Dolt keyless | Prolly keyed | Dolt keyed |
|---|---|---|---|---|
| First commit of the 1M rows | 1,579 | 0.9 | 1,411 | 1.3 |
| Commit after one row changed | 0.70 | 0.30 | 0.54 | 0.35 |
| `dolt_status` after one row | 0.84 | 0.43 | 0.38 | 0.27 |
| `dolt_diff` HEAD–WORKING after one row | 0.80 | 0.50 | 0.47 | 0.25 |
| Commit after 10,000 rows updated | 223 | 0.9 | 37 | 1.1 |
| `dolt_diff` of two commits 10,000 rows apart | 51 | 47 | 2.8 | 7.6 |
| `dolt_status` after 10,000 rows updated | 249 | 0.5 | 65 | 0.5 |
| Hard reset of 30,000 changed rows | 444 | 0.4 | 203 | 0.8 |
| Make a branch | 0.12 | 0.41 | 0.11 | 0.35 |
| Check out a new branch (first use) | 115 | 0.13 | 217 | 0.15 |
| Check out another branch and back, nothing changed | 95 | 0.21 | 197 | 0.23 |
| Check out with 10,000 rows changed | 357 | 0.53 | 70 | 0.53 |
| Fast-forward merge of 10,000 changed rows | 459 | 0.49 | 111 | 0.54 |
| Three-way merge, 10,000 rows changed on each side | 357 | 204 | 77 | 15 |
| Three-way merge, one row changed on each side | 2.0 | 0.8 | 1.9 | 0.7 |
| Cherry-pick of 10,000 changed rows | 5,156 | 140 | 181 | 15 |
| `count(*)` of `dolt_log` | 0.35 | 0.32 | 0.28 | 0.21 |

### Space after the run (bytes)

| | Heap | Prolly keyless | Dolt keyless | Prolly keyed | Dolt keyed |
|---|---|---|---|---|---|
| Database on disk | 94 M | 1,304 M | 1,248 M | 247 M | 46 M |

Neither history store had been garbage-collected (prolly has no GC yet,
step 7; Dolt's `dolt gc` wasn't run). Prolly's keyless total is 1,132 M of
chunk journal and 143 M of pages.

## 3. What the numbers say

**Dolt pays for history at write time; prolly at commit and move time.**
Dolt's working set is its tree, so a commit, a status, a branch switch, a
reset or a fast-forward only names a root: under a millisecond whatever
changed. Prolly writes as heap does and folds the changes into a tree when
it commits: updating 10,000 rows and committing them costs prolly 25 + 37 =
62 ms keyed, Dolt 1,155 + 1 = 1,156 ms. Moving a branch's pages to another
commit (reset, fast-forward, checking out a dirty branch) applies the
difference as rows, about 7–11 µs a row keyed; Dolt only moves a pointer.

**Keyless tables hurt Dolt's history.** Their hashes put rows in random
order, so a range of changed rows touches as many leaf chunks as rows,
where the same range keyed touches a few: keyless, Dolt's journal grew to
27 times the keyed run's, a 10,000-row diff took 6 times as long, and
merges and cherry-picks of 10,000 rows 13 and 10 times as long. Its
merges of keyless tables also never use the subtree-splicing fast path
(`canFastMergeProllyTrees := !keyless && …`,
`libraries/doltcore/merge/merge_prolly_rows.go`).

**Keyless tables hurt prolly's folds, through one gap.** With #136 a
keyless table folds from its changed pages, but a fold made in the
transaction that wrote the rows leaves the table marked unsettled, and
the next fold reads it in full. Every merge, cherry-pick and revert
commits in the transaction that applied its rows, so after each one the
next fold of a keyless table is a full one: in this run the cherry-pick
(5.2 s, of which most is `fold_all`'s sort and tree build) and every
checkout (95 ms) folded the whole table. The cherry-pick alone, after
plain commits, folds only its pages (counted: 38 pages, no full table).

**Checkouts fold even when nothing changed.** Leaving a branch checks
whether its working set equals its head by folding it; a branch with
nothing changed takes no checkpoint, so its baseline never advances and
every checkout folds every change since the branch's last commit's
baseline again: 95 ms keyless (full folds), 197 ms keyed (lookups of the
carried and changed keys). "Has anything changed?" should be O(1) a table
(`10-complexity-requirements.md`).

**The first commit folds every row** (1.4–1.6 s for a million), once; Dolt
had paid that cost while loading (6.2 s against prolly's 0.17 s).

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
   fast-forward and a dirty checkout here also paid for folds.
5. **The journal grows with every full fold** (1.1 G here, keyless): likely
   the trees rebuilt by full folds written again, unverified; step 7's GC
   reclaims them, and 1 removes most of them.
