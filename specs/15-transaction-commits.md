# 15. A Dolt commit for every SQL transaction

**Status: proposed 2026-10-18; code references at `1603edcaa2`. Nothing
here is built.**
Dolt's `@@dolt_transaction_commit` makes every SQL `COMMIT` that changed
data also make a Dolt commit. This describes exactly what Dolt does, and
proposes the same mode for versioned schemas in two phases: phase 1
(#228) folds the working set at pre-commit, as `dolt_commit()` does;
phase 2 (#229) commits only the transaction's own row changes, so
concurrent writers on one
branch neither capture each other's changes nor fail each other's
commits.

## 1. What Dolt does

Dolt 2.4.2 (source, `51cc4458b7`), `go/libraries/doltcore/sqle`.

**Settings.** `@@dolt_transaction_commit` (boolean, session or global,
default off) and `@@dolt_transaction_commit_message` (default
"Transaction commit") (`system_variables.go:125-140`,
`dsess/variables.go:40-41`). The server config's
`behavior.dolt_transaction_commit` sets the global. Author and committer
come from `@@dolt_author_*` and `@@dolt_committer_*`
(`dsess/variables.go:230-260`).

**At `COMMIT`** (`DoltSession.CommitTransaction`, `dsess/session.go:535-645`):

1. No dirty working set in the session: nothing happens, no commit.
2. Dirty working sets on more than one branch or database: the commit
   fails, "Cannot commit changes on more than one branch / database".
3. With the mode on, the dirty branch must be the one checked out, else
   "no changes to dolt_commit on branch X" (`validateDoltCommit`).
4. The session's roots are staged in full: every table and schema
   (`actions.StageAllTables`), so `dolt_add` is never needed.
5. A pending commit is built from the staged root, with the merge parent
   if a merge is in progress. Nothing staged (only ignored tables
   changed, say): the working set is committed without a Dolt commit; no
   empty commits.
6. The write (`DoltTransaction.doCommit`, `dsess/transactions.go:395-520`)
   holds a lock per working set and retries an optimistic write:
   - If the branch's working set moved since the transaction began, the
     working and staged roots are merged three ways with the transaction's
     start as the base (`mergeRoots`). Conflicts fail the commit unless
     the session resolves them by setting.
   - If the branch's **head** moved since the transaction began, the
     staged root is merged with the current head, the transaction's
     starting head as the base (`doltCommit`, `transactions.go:195-280`).
   - The working set and the commit are written in one atomic update.

**What the commit holds.** The transaction's own changes, merged with
the head and with whatever other sessions *staged*. Another session's
SQL-committed but unstaged changes stay in the working set and out of the
commit (the "two clients" test, `enginetest/dolt_transaction_commit_test.go`:
client c's row is in `WORKING`, not in `HEAD`).

**Autocommit.** Every autocommitted statement is a transaction, so every
one makes a commit. Dolt's tests and docs recommend `autocommit = 0`.

**Rollback.** Nothing is written; the commit and the SQL transaction are
one write.

## 2. What prolly has today

- `dolt_commit()` on versioned schemas (refs "copies") holds the refs
  until the transaction ends (`lock_refs()`, `refs_copies.c:283-305`: it
  moves the chunk store's root to itself, so commits are serialized
  **per database**), then folds the session branch from its baseline
  under a snapshot that sees the head (`commit_branch()`,
  `refs_copies.c:974-990`).
- If another transaction moved the head since this one's snapshot,
  `see_head()` takes a newer snapshot under read committed, and fails the
  transaction with a serialization failure under repeatable read and
  serializable (`head_moved_unseen()` and `see_head()`,
  `refs_copies.c:406-444`).
- The commit's tree is written with the transaction; the commit record
  carries the new root; a rollback, or a rollback to a savepoint taken
  before the commit, undoes it.
- The fold commits the branch's working set as the snapshot sees it,
  which includes other sessions' SQL-committed changes (the scenario
  tests of 2026-10-17: under read committed, another session's committed
  row is in the commit).
- There is no staging (user decision, 2026-10-17): a commit commits
  everything in the working set.
- `dolt.author` and `dolt.allow_commit_conflicts` are the existing
  settings (`prolly_dolt.c:69-107`).

## 3. The mode

**Settings.**
- `dolt.transaction_commit` (boolean, default off; settable per
  database, role and session, so `ALTER ROLE app SET
  dolt.transaction_commit = on` puts one application in the mode).
- `dolt.transaction_commit_message` (default `Transaction commit`).
  `SET LOCAL` gives one transaction its own message.

**At commit**, from a pre-commit hook that runs after deferred triggers
and before the chunk store's own pre-commit work (callbacks run in the
reverse of their registration order, so the mode registers after the
chunk store or the chunk store calls it):

1. The transaction changed nothing in a versioned schema: no commit.
   "Changed" is a flag set by the `prolly` table AM's write paths
   (`tuple_insert`, `multi_insert`, `tuple_update`, `tuple_delete`, and
   what rewrites or truncates a table) and where a branch-aware catalog
   row is written, never by page changes a read makes (pruning, hint
   bits, visibility-map bits), so read-only and unversioned transactions
   pay nothing.
2. It changed more than one branch (it checked out another and wrote
   there): the commit fails, as Dolt's does.
3. On a detached head: the commit fails, as Dolt's does.
4. Unresolved merge conflicts or constraint violations: the commit fails
   as `dolt_commit()` does, unless `dolt.allow_commit_conflicts` is set.
5. A merge in progress: the commit is the merge commit, with both
   parents, as `copies_commit()` makes it.
6. Otherwise a Dolt commit is made by phase 1's or phase 2's rule, with
   the message, `dolt.author`, and the transaction's start time, as
   `dolt_commit()` uses.
7. A commit that would hold no change (the transaction inserted a row and
   deleted it) is not made, and the transaction commits.

Any error fails the `COMMIT`, which rolls the transaction back: the SQL
transaction and its Dolt commit are always both made or both not.

**`dolt_commit()` inside the transaction.** Its commit stands. The
transaction's commit then holds only what was changed after it, and is
not made if nothing was. Phase 1 gets this from rule 7: its fold finds
the head holding what came before. Phase 2 marks its list of recorded
changes (part 5) at the `dolt_commit()`: entries before the mark are in
that commit and left out of the delta, and the "changed" flag is reset.
The mark is kept per subtransaction level, so a rollback to a savepoint
taken before the `dolt_commit()`, which undoes its commit, moves the mark
back and the entries before it count again.

**Moving heads inside the transaction** (`dolt_merge`, `dolt_reset`,
`dolt_cherry_pick`, `dolt_revert`): they make their own commits as they
do now, and the rows the applier writes for them are not the
transaction's changes (phase 2 doesn't record them).

**Two-phase commit.** `PREPARE TRANSACTION` is refused when the mode
would make a commit. Supporting it means holding the refs from `PREPARE`
to `COMMIT PREPARED` and carrying the root in the latter's record; that
is a later decision, shared with `dolt_commit()` in a prepared
transaction.

**Autocommit.** As in Dolt, every autocommitted statement that writes a
versioned schema makes a commit. Recommend explicit transactions.

## 4. Phase 1: fold at pre-commit

The commit is `copies_commit()` run at pre-commit: hold the refs, fold
the branch from its baseline under a snapshot that sees the head, write
the commit, move the head and baseline. Nearly all of it exists.

What it gives, by how the branch is written:

| Writers on the branch | Read committed | Repeatable read, serializable |
|---|---|---|
| One (spec #148's `owned` and branch modes) | Exactly the transaction's changes | Same: only the writer moves the head |
| Many, all in the mode | Exactly the transaction's changes: everything committed before is already in the head | A transaction fails at `COMMIT` with a serialization failure if another committed to the branch since its snapshot |
| Many, some not in the mode | The transaction's changes and the others' committed changes | As above, and the others' changes are captured |

The last row differs from Dolt, which captures only what others staged;
prolly has no staging. Phase 2 fixes both the capture and the failures.

**Cost.** One fold per writing transaction, of what changed since the
last commit (with every writer in the mode, the transaction's own
changes), plus the commit's tree and refs writes and a chunk-store
append. The refs are held from pre-commit to the end of the transaction,
so **commits in the mode are serialized per database** for that whole
critical section, including a WAL flush for the chunk append and another
for the commit record. Dolt serializes per working set. This is the
throughput ceiling to measure first.

## 5. Phase 2: commit the transaction's own changes

The commit is the head the transaction finds when it holds the refs,
plus the transaction's own row changes, wherever its snapshot was.

**Recording changes.** The `prolly` table AM, a wrapper over heap's
routine, records the TID of every tuple version its transaction makes
through `tuple_insert`, `multi_insert` and `tuple_update`, and the old
TID of every update and delete, in a per-transaction list that spills to
a tuplestore. At commit, entries whose `xmin` (new versions) or update
xid (old versions; `HeapTupleGetUpdateXid`, ignoring lock-only `xmax`) is
not `TransactionIdIsCurrentTransactionId` are dropped, which drops
aborted subtransactions' changes without bookkeeping per savepoint. The
list is then deduplicated by TID and role (new version or old) before
the delta is built: the same TID can be recorded twice and pass the
filter both times (an old version updated in a subtransaction rolled
back, then updated again; an aborted version's slot pruned and reused by
a later insert of the transaction), and counting it twice would put a
keyless table's count, or a delete, in history twice.

**The delta.** For each table: old versions are deletes of their key,
new versions are puts of key and canonical row. A key both deleted and
put is one put; a key put then deleted within the transaction is
nothing. Keyless tables: each old version is a count of −1 for its
content's key, each new version +1. Rows are read from the pages: old
versions are still there (the transaction hasn't committed, so nothing
pruned them), and their out-of-line values with them, so the full fold's
fallback for out-of-line keys on old versions is never needed.

**Applying it.** Holding the refs, the delta is applied to the current
head's tree for each table, with the fold's tree builder, and the result
is committed with the head as its parent. No snapshot is taken and none
is checked, so phase 1's serialization failures go away: the heap's row
locks, unique indexes and isolation level already decided which
concurrent row changes may both commit, and they commit to history in
the order they commit to the heap (the refs are held to transaction end).

A put replaces the head's row whatever it was: the transaction's new
version is the newest committed version of that row (its row lock made
sure of that), so it is the row's value after the commit, in the working
set and in history alike.

**Falling back to a fold.** A change the delta can't express folds the
table (or the branch) as phase 1 does:

- DDL in a versioned schema: the catalogs are folded, and tables whose
  shape changed.
- A table created, truncated or rewritten in the transaction (`TRUNCATE`,
  a rewriting `ALTER TABLE`, `COPY FREEZE`'s frozen tuples), or one the
  head doesn't have.
- A keyless count that would fall below zero: another session's
  uncommitted-to-history copy was deleted. The fold commits that table as
  the transaction sees it.
- A merge in progress: the merge commit commits the working set.
- A recorded change list over a size limit: the fold is cheaper.

**The baseline.** Phase 2 commits don't advance the branch's baseline:
the next full fold (`dolt_commit()`, `dolt_status`, a checkout's clean
check) reads every page changed since the last full fold, finds the rows
already in the head unchanged, and advances it. Advancing the baseline
from a delta commit, when nothing outside the mode wrote the branch
since, is an open question.

**What it gives.** The commit holds exactly the transaction's row
changes, at every isolation level, whoever else writes the branch. In a
branch also written outside the mode, the others' changes stay in the
working set, as in Dolt, and `dolt_diff('HEAD', 'WORKING')` shows only
theirs. Such a head can hold rows the working set never held together
(a child row whose parent another session inserted outside the mode);
foreign keys are not rechecked against the head, as Dolt doesn't, and
`dolt_verify_constraints()` finds them.

## 6. Testing

- The Dolt cases: `enginetest/dolt_transaction_commit_test.go` (one
  client, two clients with a third outside the mode, autocommit, commit
  messages) and the `dolt_transaction_commit` cases of
  `dolt_transaction_queries.go`, ported into `vcscenario` as feature-gated
  ports (#163).
- The rollback cases, in the mode: `ROLLBACK`, `ROLLBACK TO SAVEPOINT`, an
  error caught in a PL/pgSQL block, an error in a deferred trigger, an
  error at commit (merge conflicts): no commit in each.
- Concurrency: N sessions in the mode inserting and updating overlapping
  keys at each isolation level; the log has one commit per committed
  transaction, in commit order, and each commit's diff is exactly its
  transaction's rows. Phase 1 under repeatable read: the expected
  serialization failures; phase 2: none beyond heap's own.
- Mixed: a session outside the mode writes; phase 2's commits don't hold
  its rows, and `WORKING` does.
- Crash after the commit record and before the head is visible; a
  standby's log.
- Throughput: pgbench (TPC-B-like and a select-only control) with the
  mode off and on, 1 to 64 clients, phase 1 and phase 2.

## 7. Open questions

1. **Commit throughput.** The refs are per database and held to
   transaction end, so every writing transaction in the mode serializes
   through the commit critical section with two WAL flushes. If the
   measurement says it matters: a narrower lock per branch with the root
   move alone serialized, or group commit (a leader moving the root once
   for a chain of queued transactions' commits).
2. **Baseline advance after phase 2 commits** (part 5).
3. **Two-phase commit** (part 3).
4. **With #148's modes.** `branch-auto` puts each writer on its own
   branch; in this mode each of its transactions is then a commit on
   that branch. Whether integration into `main` (`dolt_integrate`) should
   also be automatic is #148's question, not this one's.
