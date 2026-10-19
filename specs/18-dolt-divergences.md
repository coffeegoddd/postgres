# 18. Intended divergences from Dolt

**Status: decided by the user 2026-10-18, as recommended (the policy
below); entries checked on `8faba578f9`.** The ways versioned schemas
differ from Dolt on purpose, each with a stable ID that Dolt test ports
cite (`corpus/dolt/ports.json`, a port's review note), so a port doesn't
re-argue a difference and a bug isn't mistaken for one. The IDs are this
file's (D1, D2, …), not the removed engine D1's name; an ID is never
reused, and an entry that stops being a divergence is marked so, not
deleted.

**Policy** (#153). A difference found while porting is fixed to match
Dolt, unless Postgres semantics force it; then it gets an entry here,
and the port cites it. The user is asked only when a decided spec
conflicts. Scope (#164): every Dolt version-control feature is in scope
except the staging area (D1); what isn't ported is D10.

Each entry: what Dolt does, what prolly does, why, and how a port
adapts.

## D1. No staging area

- **Dolt:** a working set and a staging area (the index). `dolt_add`
  stages tables; `dolt_commit` commits what is staged (`-a`/`-A` stage
  first); soft and mixed resets move the index; `dolt_status.staged`
  tells the two apart. About 199 of Dolt's engine cases touch staging.
- **Prolly:** a commit records the branch's whole working set, so every
  change is staged as it is made (user, 2026-10-17). `dolt_add` does
  nothing but check its arguments; `dolt_commit` accepts `-a` and `-A`
  and needs neither; `dolt_reset` does hard resets only.
  `dolt_status.staged` is true for every row except those whose status
  is `conflict` or `not covered` (objects a commit can't hold yet,
  which are listed and unstaged).
- **Why:** the user's decision (2026-10-17). A staging area would be a
  second state of each branch beside the working set in Postgres's
  pages, for every fold, status and diff to track.
- **Port:** drop `dolt_add` calls whose only effect is staging; map
  Dolt's `staged` column as above; a case whose assertion is about the
  index (a soft or mixed reset, `--cached`, partially staged tables) is
  N, not applicable. Where Dolt runs `dolt_add` to mark conflicts
  resolved before `--continue`, prolly resolves with
  `dolt_conflicts_resolve()` (or the rows fixed by hand and resolved),
  then `dolt_commit` or `--continue`.

## D2. Only versioned schemas are versioned

- **Dolt:** the whole database is versioned.
- **Prolly:** only objects in `CREATE SCHEMA … VERSIONED` are per branch
  and in commits (user, 2026-10-16, Doltgres's scope). Ordinary schemas,
  shared catalogs, roles and large objects are the cluster's or the
  database's, on every branch; dependencies go only from versioned
  objects to shared ones (spec 11, part 1).
- **Why:** Postgres's own objects, extensions and ordinary tables keep
  working as they do in Postgres.
- **Port:** create the case's tables in a versioned schema (the adapter
  does `CREATE SCHEMA public VERSIONED`); a case that relies on an
  unversioned object being versioned is rewritten or marked N.

## D3. Return shapes

- **Dolt:** procedures return result rows (`CALL DOLT_COMMIT(...)` gives
  a hash column; merge gives `hash, fast_forward, conflicts, message`);
  diff and conflict tables flatten both sides into `from_<col>` and
  `to_<col>` columns.
- **Prolly:** the `dolt_*` calls are functions returning `text[]`
  (`SELECT dolt_commit('-m', 'x')` gives `{<hash>}`). Diff and conflict
  rows are the table's row type: `dolt_diff(from, to, NULL::t)` returns
  `(diff_type, from_row t, to_row t)`; `dolt_conflicts(NULL::t)` returns
  `(base_row, our_row, their_row, our_diff_type, their_diff_type)`.
- **Why:** Postgres functions, not MySQL procedures; row composites keep
  the column types and work for any table without generated column
  names.
- **Port:** compare array elements by position; read `(from_row).col`
  where Dolt reads `from_col`.

## D4. Errors are SQLSTATEs

- **Dolt:** errors are MySQL error codes with Dolt's message text, and
  its tests match the text.
- **Prolly:** errors are Postgres SQLSTATEs with Postgres-style
  messages: `0A000` (not supported), `22023` (an invalid argument),
  `55000` (not in the needed state: nothing to commit, uncommitted
  changes), `40001` (serialization failure: a moved head, unresolved
  conflicts at `COMMIT`), `42704` (no such branch or commit), `42501`
  (privileges).
- **Why:** clients and drivers handle Postgres errors by SQLSTATE.
- **Port:** assert the SQLSTATE (`expect.sqlstate`), never the text.

## D5. Transactions

- **Dolt:** version-control calls write the session's working set, and
  some act outside the SQL transaction: `dolt_reset('--hard')` commits
  the active transaction first. Conflicts in the working set fail the
  SQL `COMMIT` unless `@@dolt_allow_commit_conflicts` is set.
- **Prolly:** every version-control call is part of its SQL transaction
  and savepoint: a rollback undoes its commit, head move or merge, and
  nothing commits the transaction early. A merge's unresolved conflicts
  or constraint violations fail the SQL `COMMIT` unless
  `dolt.allow_commit_conflicts` is set. `WORKING` in `dolt_status` and
  `dolt_diff` is the branch's working set as committed by SQL
  transactions, so neither shows the current transaction's own
  uncommitted writes (`dolt_commit` does fold them).
- **Why:** Postgres's atomicity: a statement's effects are the
  transaction's.
- **Port:** a case asserting that reset committed the transaction is
  deferred with this ID (`ports.json` has one); add an explicit `COMMIT`
  only where the assertion isn't about that. Reads of `WORKING` after
  writes in the same transaction move after its `COMMIT`.

## D6. Merges

- **Dolt:** `AUTO_INCREMENT` counters are kept outside the working set,
  across branches, so a hard reset or a merge never hands a value out
  again (Doltgres keeps sequences global too). Schema conflicts can be
  recorded in `dolt_schema_conflicts`. A merge into uncommitted changes
  is refused when they touch a table the merge changes.
- **Prolly:** sequences are versioned objects, per branch and in each
  commit (user, 2026-10-16): a merge gives each sequence the larger of
  its two values, and a hard reset restores the commit's value, so a key
  generated after it can repeat one handed out before. Schema conflicts
  are errors naming the objects; the merge doesn't happen. A merge,
  cherry-pick or revert into uncommitted changes is refused when it
  would change a table, sequence or materialized view with uncommitted
  changes, as Dolt's is, and also whenever definitions change on either
  side, even without a conflict.
- **Why:** a sequence is a schema object in Postgres, versioned with its
  schema; Postgres DDL can't be left half-merged in the catalogs.
- **Port:** expected generated keys follow the commit's sequence value
  (`dolt-reset-identity`, deferred with this ID); schema-conflict cases
  expect the error, not rows; dirty-merge cases with definition changes
  expect the refusal.

## D7. Hashes

- **Dolt:** commit ids are 32-character base32 strings of a 20-byte
  hash.
- **Prolly:** commit ids are 64 hex digits of SHA-256, canonical: the
  same history gives the same ids on every cluster.
- **Why:** prolly's own format (Built for Postgres, not ported); ids
  are never compared with Dolt's.
- **Port:** capture ids from the calls (typed steps) and compare them
  with each other, never with Dolt's literals; check their shape, not
  their value.

## D8. Concurrency

- **Dolt:** each session has its own working set; at `COMMIT` the
  transaction's changes are merged into the branch's working set, and a
  conflicting concurrent change fails the commit.
- **Prolly:** Postgres's MVCC and row locks on the branch's shared
  working set (locks per branch, #91): a second writer to a row waits
  for the first, then proceeds (read committed) or fails with `40001`
  (repeatable read, serializable). The writer modes (spec 11, 6.2) can
  make one writer per working set.
- **Why:** Postgres's concurrency semantics are what this fork keeps.
- **Port:** multi-session cases are rederived under Postgres semantics
  (#165): blocking and serialization as isolation specs, sequential
  interleavings as multi-session scenarios. A forced delete
  (`dolt_branch -D`, `-df`) of a branch another session is on is
  refused (55006), where Dolt deletes it under that session: that
  session's catalog is the branch's copies, as Postgres won't drop a
  database in use. A forced rename (`-mf`) goes ahead, as Dolt's does.

## D9. Reverting several commits (no longer a divergence)

- **Dolt:** `dolt_revert` takes several commits and makes one revert
  commit for each, newest first, stopping at the first that conflicts.
- **Prolly:** the same, since `bb68288278`; `--abort` and `--continue`
  work over the series.
- **Status:** checked against Dolt's source and changed to match; kept
  so the ID isn't reused. The ports are P (`dolt-revert-one-commit-each`,
  `dolt-revert-series-stops`, `dolt-revert-abort-series`).

## D10. Not ported

- **Staging-area features** (D1): `dolt_workspace_*`, `dolt_rm`, soft
  and mixed reset, `--cached`.
- **MySQL-only syntax and types:** `CALL`, backtick identifiers, MySQL
  DDL options, `SET @@…` variables, MySQL-only types; a case translated
  to Postgres keeps its version-control assertions, and one that tests
  only MySQL behavior is N.
- **Dolt's storage internals:** chunk journal files, noms formats, table
  file names, `dolt_gc` internals and their outputs.
- **CLI-only output** (bats tests that check printed text).

Every other Dolt version-control feature is in scope; one prolly lacks
yet (diff system tables, `dolt_history_<t>`, tags, `AS OF`, rebase,
stash, …) is F, ported with the feature (#163).

## D11. Branch-qualified databases are connections

- **Dolt:** `USE mydb/branch` (or a revision) switches a session's
  database; `` `mydb/branch`.t `` reads another branch's table in the
  same query; `mydb` alone resolves to the branch the session last
  checked out; `information_schema` is the base database's whatever
  the session uses, with each revision database's tables under its own
  `table_schema`.
- **Prolly:** a connection names `dbname/branch` (as Doltgres's do) and
  starts on that branch; `dbname` alone starts on the database's
  default branch. A connection to a branch that doesn't exist is
  refused, SQLSTATE 42704, as `dolt_checkout`'s is. A session's
  catalogs, `information_schema` included, are its branch's, as in
  Doltgres.
- **Why:** Postgres has no `USE` and no cross-database references; a
  connection is a session, so nothing carries from one to the next.
- **Port:** `USE` becomes the scenario's `connect` operation, a new
  connection for that session; a cross-database reference becomes the
  same query on a second session connected to that branch. Assertions
  that `mydb` follows the session's last checkout expect the default
  branch; `information_schema` assertions expect the connection's
  branch's columns. `DATABASE()` and `SHOW DATABASES` are MySQL's
  (D10).
