# Dolt compatibility and test adaptations

This catalog records the behavior and test adaptations associated with the
stable D1–D25 IDs in `ports.json`. IDs are never reused or renumbered; when
a difference is removed, mark its entry as resolved and update the ports.
These are divergence IDs, unrelated to the removed engine named D1.

This catalog was moved from the former design specs. It is a record of
existing contracts and adaptations, not a claim that all differences are
permanent. GitHub issues track plans and changes:

- [#153](https://github.com/coffeegoddd/postgres/issues/153) tracks parity:
  every Dolt version-control feature is in scope except the staging area.
  Fix differences unless PostgreSQL semantics require an adaptation. A new
  required adaptation gets the next ID here (decided in its issue), and the
  ports cite it.
- [#701](https://github.com/coffeegoddd/postgres/issues/701) revisits D6's
  schema-conflict errors.
- [#702](https://github.com/coffeegoddd/postgres/issues/702) revisits D12/D13's
  constraint-violation rows and artifacts.
- [#699](https://github.com/coffeegoddd/postgres/issues/699) and
  [#700](https://github.com/coffeegoddd/postgres/issues/700) changed D18's
  column matching and D17's table renames to follow identity.
- [#710](https://github.com/coffeegoddd/postgres/issues/710) tracks documenting
  PostGIS NaN points' send/receive round trip; it is not assigned an ID here.

Change the relevant entry and its tests together when an issue changes the
behavior. Missing features in D10 remain work to implement, not exceptions
to the parity goal.

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
  and needs neither. `dolt_reset('--hard')` moves the head and the
  working set; `--soft` and the plain form (Dolt's soft and mixed
  resets, one without an index) move only the head, so what differs from
  it shows as changes (#493); a protected branch, which has none, refuses
  one that would leave it with some.
  `dolt_status.staged` is true for every row except those whose status
  is `conflict` or `not covered` (objects a commit can't hold yet,
  which are listed and unstaged). `dolt_stash('push', ...)` sets the
  whole working set aside, so new tables with the rest (Dolt's leaves
  untracked ones without `-u`); `-u` and `-a` are accepted (#498). A
  pop or apply merges into uncommitted changes as Dolt's does, refused
  only where rows conflict (#585), or where definitions change (D6).
- **Why:** the user's decision (2026-10-17). A staging area would be a
  second state of each branch beside the working set in Postgres's
  pages, for every fold, status and diff to track.
- **Port:** drop `dolt_add` calls whose only effect is staging; map
  Dolt's `staged` column as above; a case whose assertion is about the
  index (what a soft or mixed reset leaves staged, `--cached`, partially
  staged tables) is N, not applicable; one about where a soft or mixed
  reset leaves the head and the working set ports. Where Dolt runs `dolt_add` to mark conflicts
  resolved before `--continue`, prolly resolves with
  `dolt_conflicts_resolve()` (or the rows fixed by hand and resolved),
  then `dolt_commit` or `--continue`. Dolt refuses a rebase's
  `--continue` while the working set has any unstaged change ("cannot
  continue a rebase with unstaged changes"), as a resolution not yet
  added is; prolly has no such state, since every change is staged as
  it is made. A port checks that assertion as `dolt_status` showing
  the resolved table staged: `staged` is false only for `conflict` (and
  `not covered`) rows, so a staged, modified row says the conflict was
  resolved. Dolt's case then runs `dolt_add` and a second
  `--continue`; the port runs the same `dolt_add` (a no-op) and a
  single `--continue`, which goes on as Dolt's second does. The case is
  ported, the divergence intended.

## D2. Only versioned schemas are versioned

- **Dolt:** the whole database is versioned.
- **Prolly:** only objects in `CREATE SCHEMA … VERSIONED` are per branch
  and in commits (user, 2026-10-16, Doltgres's scope). Ordinary schemas,
  shared catalogs, roles and large objects are the cluster's or the
  database's, on every branch; dependencies go only from versioned
  objects to shared ones ([historical schema design](https://github.com/coffeegoddd/postgres/blob/28cca9345d/specs/11-pages-and-dolt-snapshots.md)).
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
  Historical enum values are translated using the saved type definitions
  into that current row type (#387). A pure rename preserves each label's
  position; surviving names must keep their positions. Additions preserve
  existing labels by name and order, so a type that only gained labels
  leaves its tables' rows reading as they did (no table changes with it). A label-only rename therefore produces
  no typed row diff; the enum's definition still changes. Actual row edits,
  additions, deletions and conflicts use the current labels on both sides.
  This includes domains, arrays, unchanged composites, enum keys and keyless
  copy counts. Between two commits the labels are followed through the
  history (#454): back from one version to the merge base, then on to the
  other, through each merge along the parent it took the enum's
  definition from (first parents otherwise; the edge from a merge's other
  parent may hold several changes at once), each commit's change read as
  a rename in place or an addition and the per-commit mappings composed.
  So a label added in one commit and another renamed in a later one, a
  chain of renames, or a renamed label's old name added again later, all
  read correctly, also across a merge commit. The working set is a change past the head (and, mid-merge,
  past the merged side), and a merge's result defines each enum as one side
  does. Where no history is known, only the two definitions are compared.
  A single commit that both renames and adds labels (or reorders/reuses
  names), a value whose label the target enum lacks, changes to enclosing
  type definitions, ranges holding renamed enums, and merges that also drop
  columns are refused with a schema-conflict error rather than guessed.
  History stores definitions, not the individual rename commands; there is
  no change to the canonical storage format (labels carry no sort-order
  identity).
  The commit tables are views over functions, as `dolt_log` is:
  `dolt_commits` (every commit a branch reaches) and
  `dolt_commit_ancestors` (each one's parents by `parent_index`, a root
  commit's a null parent at index 0, as Dolt's) (#588). `dolt_commits`
  has `commit_hash`, `committer`, `email`, `date` and `message`, not
  Dolt's `author`, `author_email` and `author_date`: a prolly commit
  records one author, which those columns would repeat.
  A table the session hasn't (dropped since, or renamed) has no row
  type, so `dolt_diff(from, to, 't')`, given the name as text, returns
  `(diff_type, from_row jsonb, to_row jsonb)` instead: each side's row
  as an object of its columns, decoded as that side's ref defines the
  table (#612, D18).
- **Why:** Postgres functions, not MySQL procedures; row composites keep
  the column types and work for any table without generated column
  names.
- **Port:** compare array elements by position; read `(from_row).col`
  where Dolt reads `from_col`; for a table that doesn't exist now, name
  it as text and read `from_row->>'col'`.

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
  `dolt.allow_commit_conflicts` is set: the `COMMIT` of the transaction
  that made them, or of any that writes to the branch while they're
  there, whoever made them, as in Dolt (#587); one that only reads
  commits. `WORKING` in `dolt_status` and
  `dolt_diff` is the branch's working set as the current transaction
  sees it, its own uncommitted writes included, as a Dolt session's
  working root includes its own (#501). A rebase whose step conflicts
  stops there, in a transaction block (or with
  `dolt.allow_commit_conflicts`), returning `{1, message}` rather than
  Dolt's error, which here would roll the stop back. Outside a block it
  is an error, and the statement rolls back: a rebase the statement
  began is gone, as Dolt aborts it, but one begun before (an
  interactive rebase's `--continue`) stays as it was before the
  statement, its plan to change or abort, where Dolt aborts it. A
  schema conflict is such an error, in a block or not.
- **Why:** Postgres's atomicity: a statement's effects are the
  transaction's.
- **Port:** a case asserting that reset committed the transaction is
  deferred with this ID (`ports.json` has one); add an explicit `COMMIT`
  only where the assertion isn't about that. An assertion reading what a
  call did before its error (Dolt's table checkout naming a missing
  table) is left out with this ID. Reads of `WORKING` after
  writes in the same transaction stay where they are.

## D6. Merges

- **Dolt:** `AUTO_INCREMENT` counters are kept outside the working set,
  across branches, so a hard reset or a merge never hands a value out
  again (Doltgres keeps sequences global too). Schema conflicts can be
  recorded in `dolt_schema_conflicts`. A merge into uncommitted changes
  is refused when they touch a table the merge changes.
- **Prolly:** sequences are versioned objects, per branch and in each
  commit (user, 2026-10-16): a merge gives each sequence the larger of
  its two values, and a hard reset restores the commit's value, so a key
  generated after it can repeat one handed out before. A table in a
  schema conflict (both sides changed it in ways that don't merge, or one
  dropped it and the other changed it) is kept as Dolt keeps it: as the
  branch has it, its rows unmerged, recorded in the merge in progress and
  listed in `dolt_schema_conflicts`; `dolt_conflicts_resolve()` refuses it
  and a commit waits for `--abort`, as in Dolt. Other schema conflicts
  (objects other than tables, renames to one name, inheritance and typed
  tables, enum labels, or a table the other side's objects use in its
  version) are errors naming the
  objects, and the merge doesn't happen; so is any schema conflict of a
  cherry-pick or revert. A merge,
  cherry-pick or revert into uncommitted changes is refused when it
  would change a table, sequence or materialized view with uncommitted
  changes, as Dolt's is. When definitions change, on either side or
  uncommitted, it goes ahead over uncommitted changes to objects it
  doesn't change (#494), refused only where both change an object, and,
  beyond Dolt, where a table with uncommitted changes uses a type the
  merge changes, where the uncommitted definitions don't fit the merged
  ones (a key dropped that a merged foreign key needs), or where they
  break a merged foreign key.
- **Why:** a sequence is a schema object in Postgres, versioned with its
  schema; Postgres DDL can't be left half-merged in the catalogs.
- **Port:** expected generated keys follow the commit's sequence value
  (`dolt-reset-identity`, deferred with this ID); table schema-conflict
  cases expect Dolt's result and `dolt_schema_conflicts` rows (the
  statements are Postgres's), other schema conflicts the error; dirty-merge cases with definition changes
  expect the refusal only in those cases.

## D7. Hashes

- **Dolt:** commit ids are 32-character base32 strings of a 20-byte
  hash.
- **Prolly:** commit ids are 40 hex digits of BLAKE3's first 20 bytes
  (64 hex digits of SHA-256 until [historical design 22](https://github.com/coffeegoddd/postgres/blob/28cca9345d/specs/22-chunk-store-parity.md)'s group A), canonical: the
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
  (repeatable read, serializable). The writer modes ([historical design 11](https://github.com/coffeegoddd/postgres/blob/28cca9345d/specs/11-pages-and-dolt-snapshots.md), 6.2) can
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

- **Staging-area features** (D1): `dolt_workspace_*`, `dolt_rm`,
  per-table resets, `--cached`.
- **MySQL-only syntax and types:** `CALL`, backtick identifiers, MySQL
  DDL options, `SET @@…` variables, MySQL-only types; a case translated
  to Postgres keeps its version-control assertions, and one that tests
  only MySQL behavior is N.
- **Dolt's storage internals:** chunk journal files, noms formats, table
  file names, `dolt_gc` internals and their outputs.
- **CLI-only output** (bats tests that check printed text).

Every other Dolt version-control feature is in scope; one prolly lacks
yet (tags, rebase, stash, ref ranges in diff functions, …) is F, ported
with the feature (#163). Diff system tables, `dolt_history_<t>` and `AS
OF` are built in D18's shapes.

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
  refused, SQLSTATE 42704, as `dolt_checkout`'s is; one naming
  `detached/...`, a commit's hidden branch ([historical design 11](https://github.com/coffeegoddd/postgres/blob/28cca9345d/specs/11-pages-and-dolt-snapshots.md)), whether or not it
  exists, is refused with 42939, as `dolt_checkout` refuses that
  name. A session's
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

## D12. Rows breaking a unique constraint

- **Dolt:** a merge writes every row it merges, one that breaks a unique
  index too, and records each row involved as a violation (the merged
  row and those it collides with), once for each unique index it breaks.
- **Prolly:** the merge doesn't write a row that would break a unique
  constraint: the branch keeps its own version of that key (none, for a
  new one). The violations are Dolt's: the row not written and each row
  it collides with, once for each unique index it breaks (an index of
  plain columns, by the index's own equality and collation; for an index
  of expressions, the row not written alone, for the index its write was
  refused for). Check and not-null
  constraints keep their breaking rows out too: those rows are recorded,
  not written. A row breaking
  an exclusion constraint (which Dolt doesn't have) is handled the same
  way, recorded as an `exclusion constraint` violation naming the
  constraint and its columns. So is a row breaking a `PRIMARY KEY` or
  `UNIQUE` constraint `WITHOUT OVERLAPS`, which Postgres enforces as an
  exclusion constraint and reports as one (SQLSTATE 23P01).
  For a keyless table, a removal cannot be paired with a rejected insert.
  If a merge that removes rows of a keyless table refuses a row it writes
  (a unique, exclusion, check, not-null or domain violation), the merge is
  refused with an error and the branch is left as it was: keeping either
  side's rows would silently lose the other's. Change the rows on one
  branch so they satisfy the table's constraints, then merge again. A
  merge that only inserts rows into a keyless table records the refused
  rows alone and keeps the rest, as for a keyed table. This does not change
  foreign-key checks performed after writing, or the handling of
  constraints added by the schema merge (D13).
- **Why:** a Postgres unique index refuses the second row (as a
  validated check or not-null constraint refuses a row breaking it); a
  table can't hold both, so there is no state in which Dolt's rows
  exist.
- **Port:** expected violations are Dolt's; expected rows follow
  prolly's: the table has one row fewer than Dolt's for each row the
  merge didn't write (a count of its rows, as `DELETE`'s, is that much
  lower).

## D13. Rows breaking a check a merge adds

- **Dolt:** when one side adds a check constraint, the merge checks the
  rows it merges against it (each row added or changed on either side);
  a row breaking it is recorded as a violation and stays in the table
  (`checkValidator` records an artifact and the row is merged as usual).
- **Prolly:** the merge checks every row of the merged table against
  it, and a row breaking it, the branch's own included, is taken out of
  the table and recorded as a violation; `violating_row` holds the whole
  row, to put back once fixed. The check is validated.
- **Not a divergence:** a NOT NULL a merge adds, as Dolt's. Dolt's
  `nullValidator` doesn't write theirs' rows breaking it and deletes
  ours' (`patchBuffer.SendKV(key, nil)`), recording each as a violation
  (its tests "adding a not-null constraint to one side" and "adding a
  not-null constraint and default value to a column"); prolly takes
  them out and records them the same way. Nor is a foreign key a merge
  adds: its breaking rows stay, recorded, in both.
- **Why:** left `NOT VALID`, the merged table's definition would be
  neither side's, and a commit of it would record a constraint theirs
  didn't have (`convalidated` false), so the check must be validated,
  and Postgres validates a check only over a table every row of which
  passes it. (The planner trusts a validated check too, but on a plain
  table only under `constraint_exclusion = on`.)
- **Port:** the row breaking a check is absent from the table and
  present in `dolt_constraint_violations` (a count of the table's rows
  is one lower for each); an assertion reading it after resolving reads
  it back from `violating_row`, or inserts it fixed. A NOT NULL case
  ports as is.

## D14. No branch is named "--"

- **Dolt:** a branch name is refused when it is empty, `HEAD`, `-`,
  a commit id, or has an empty `/` component; `--` is accepted, so
  `dolt_checkout('-b', '--', 'main')` (its argument parser takes `--`
  as `-b`'s value) makes and checks out a branch named `--`.
- **Prolly:** `--` is refused as a branch's name, as git's
  `check-ref-format` refuses it (git refuses any name beginning with
  `-`; names like `-dash` are still accepted here, as in Dolt). An
  option's value is taken as Dolt takes it, `--` included, so
  `dolt_commit('-m', '--')` commits with the message `--`.
- **Why:** the user's expectation in #314: `--` ends the options, and a
  branch named `--` could be named only after another `--`.
- **Port:** a case that makes a branch named `--` expects the
  `invalid branch name` error (SQLSTATE 42602) instead.
- **Tables after `--`** (#494): `dolt_checkout('--', ...)` names tables,
  as Dolt's does, their uncommitted changes discarded; one name there
  that is a branch and no table checks out the branch, so a branch named
  like an option (`-dash`) can still be checked out. Before `--` is a
  ref where it names one, as Dolt's: `dolt_checkout('b', '--')` checks
  out branch `b` though a table is named `b`, and
  `dolt_checkout('HEAD~1', '--', 't')` takes `t` as that commit has it;
  a name there that is no ref is a table, so `dolt_checkout('t', '--')`
  and `dolt_checkout('t1', '--', 't2')` check them out from the head.
  `.` names the tables the head (or the ref)
  has: a table only the working set has stays, as Dolt's untracked table
  does (the head stands for Dolt's index, D1). A name that matches no
  table fails the whole call (D5), where Dolt checks out the others,
  commits that, then errors.

## D15. Schema diffs cover every versioned object

- **Dolt:** `dolt_diff_summary` and `dolt_schema_diff` list tables, by
  their unqualified names, `''` for the side a table isn't on. Their
  arguments must be literals (or bind variables): `concat(...)`,
  `hashof(...)` or `lower(...)` as an argument is an error. A table
  whose primary key changed is refused by the three-argument summary
  and skipped, with a warning, by the two-argument one. An
  `AUTO_INCREMENT` counter is part of its table's schema, so raising it
  changes the table's schema.
- **Prolly:** both functions list every kind a commit holds (tables,
  views, materialized views, routines as `schema.f(args)`, types,
  sequences, versioned schemas), named qualified as `dolt_status` names
  them (`public.t`, as Doltgres does), `''` for a missing side. A
  serial's or identity column's sequence is an object of its own, so a
  key handed out changes the sequence's data (its value), not the
  table (D6). A name argument is qualified or found on the search path
  (Doltgres finds an unqualified name in any schema). Arguments are any
  expressions of type text, as for any Postgres function; a wrong count
  or type is `42883` (no such function), a NULL ref `22004`.
- **Why:** [historical design 21](https://github.com/coffeegoddd/postgres/blob/28cca9345d/specs/21-schema-diffs.md)'s decision (user, 2026-10-19): the other kinds are
  versioned too, and leaving them out would hide changes `dolt_status`
  shows. Functions resolve by argument types in Postgres.
- **Port:** expect qualified names, and the rows of the other objects a
  case's statements change (a versioned schema made, a sequence moved);
  an argument Dolt refuses as not a literal is evaluated, and the
  assertion expects the result. A primary-key-change case is deferred
  until it is decided whether prolly should refuse it too (its summary
  reads no rows, so it can list the table as modified).

## D16. Schema diffs give Postgres DDL

- **Dolt:** `dolt_schema_diff`'s statements are MySQL's `SHOW CREATE
  TABLE` (backquoted names, `ENGINE=InnoDB ...`); Doltgres's are a
  Postgres-like `CREATE TABLE` with the key inline.
- **Prolly:** each side is rendered from that version's canonical
  description, never the catalogs, as the statements a move would run,
  one per line: a table is `CREATE TABLE schema.t (cols);` with the
  columns' defaults and collations, then `ALTER TABLE ... ADD CONSTRAINT
  ...;` for each NOT NULL, key, check and foreign key, then its indexes
  (`CREATE INDEX ... USING btree (...)`), row security, policies,
  triggers, rules and statistics; other kinds the statement that makes
  them.
- **Why:** Postgres's DDL, from what the commit holds ([historical design 21](https://github.com/coffeegoddd/postgres/blob/28cca9345d/specs/21-schema-diffs.md), 4.2);
  constraints are named objects in Postgres, so they are statements of
  their own.
- **Port:** expected statements are prolly's rendering of the case's
  tables; which rows appear, and which side is empty, are Dolt's.

## D17. Renames are matched by identity

- **Dolt:** a table renamed is matched by its columns' tags, which
  survive renames, so a table renamed and changed in one commit (rows
  added, say) is one `renamed` row.
- **Prolly:** a table keeps its identity when renamed or moved to another
  schema (#700): a commit records it beside its root, never in it. A
  table renamed is one `renamed` row (`schema_change` true, as Dolt's,
  and `data_change` if its rows changed too), in summaries, statistics
  and row diffs, by either name, as in Dolt; an unrelated table made with
  its old name is another, added. Tables alike but unrelated, one dropped
  and another made, are a drop and an add. Other objects (views,
  routines) have no identity: a dropped and an added one of equal
  definitions (but for their names), the only such pair, are one
  renamed.
  `dolt_status` lists tables by name, so a table renamed there is
  `deleted` and `new table`. `dolt_at` and `dolt_history_table` read a
  version's table by its name, as Dolt's `AS OF` and `dolt_history_<t>`
  do (its columns by identity, D18). `dolt_diff_stat` asked by a renamed
  table's old name names the table as the newer version does.
- **Why:** identity beside the root, so the same tables and rows are the
  same root however they were named; Postgres names a statistic's table
  by the version that has it.
- **Port:** a case renaming a table and changing its rows in one commit
  expects Dolt's `renamed` row.

## D18. Per-table diff and history tables are functions

- **Dolt:** `dolt_diff_<table>` and `dolt_history_<table>` are system
  tables, one per table, and `SELECT … AS OF 'ref'` reads a table at a
  commit; the `dolt_diff` system table lists each commit's changed
  tables.
- **Prolly:** Postgres can't make a relation per table on the fly, so
  each is a function taking the table's row type, as `dolt_diff` does:
  `dolt_diff_table(NULL::t [, from_ref, to_ref])` (columns `to_row`,
  `from_row` of `t`'s row type, the commits, their dates, `diff_type`),
  `dolt_history_table(NULL::t)` (`row`, `commit_hash`, `committer`,
  `commit_date`) and `dolt_at(ref, NULL::t)` (`SETOF t`; its planner
  support estimates its rows from the commit's tree, [historical design 21](https://github.com/coffeegoddd/postgres/blob/28cca9345d/specs/21-schema-diffs.md)). The `dolt_diff`
  view, over `dolt_diff()`, is the system table, every kind of object
  listed (D15). Rows of other versions are decoded as the table is
  defined now, their columns matched by identity, which renames keep,
  as Dolt's column tags are (#699): a column renamed reads its values in
  versions before the rename. A column
  dropped changes no row (the rows read the same); one dropped and
  another of its name added has the dropped one's identity, as Dolt's
  tags do (unless the dropped one had been renamed into the name: then
  the new one is another column). A column of another type in an older version is converted to
  today's type (text `'3'` reads as integer 3, where Dolt gives NULL).
  History of an older folded format (17 or before) isn't read: its rows
  are unreadable here. A table's
  history is followed by its name: one dropped and made again has both
  tables' rows. Each also takes the table's name as text, for a
  table the session hasn't (#612): `dolt_diff_table('t' [, from_ref,
  to_ref])` and `dolt_history_table('t')`, the same columns with the
  rows as `jsonb`, each version's decoded as that version defines the
  table, read from its tree ([historical design 21](https://github.com/coffeegoddd/postgres/blob/28cca9345d/specs/21-schema-diffs.md), part 8).
- **Why:** Postgres's catalogs; the functions keep a table's own row
  type. The canonical form records no column tags.
- **Port:** `SELECT * FROM dolt_diff_t` becomes `dolt_diff_table(NULL::t)`
  with `(to_row).col` for `to_col`; `AS OF 'r'` becomes `dolt_at('r',
  NULL::t)`; `dolt_history_t` becomes `dolt_history_table(NULL::t)` with
  `(row).col`. Expected rows follow the reading by identity, as defined now. A case reading
  a table that doesn't exist now names it as text and reads
  `to_row->>'col'`.

## D19. A primary key changed is a modified table

- **Dolt:** a table whose primary key changed between two refs is
  refused by `dolt_diff_summary(from, to, t)` and left out, with a
  warning, by the two-argument form: its row diff can't be read.
- **Prolly:** it is `modified`, its rows and definition changed: rows
  are matched by name, not by key tags, and the summary reads no rows.
- **Why:** nothing in the canonical form makes the change unreadable.
- **Port:** cases expecting the refusal or the warning expect the row.

## D20. Cells modified count what differs

- **Dolt:** `dolt_diff_stat` counts a column added since as a changed
  cell of every modified row.
- **Prolly:** a modified row's cells are compared decoded, as the table
  is defined now; a column added since differs only where the values
  do.
- **Why:** the canonical form records no column tags to tell an added
  column from a value changed.
- **Port:** a case adding a column and modifying rows in one commit
  expects the cells whose values differ.


## D21. A value that doesn't convert is shown null, and compared so

- **Dolt:** an old value that doesn't convert to its column's type now
  (or doesn't fit it: "Truncated varchar(10) value") is shown null with
  a warning per value; a row whose stored value changed is modified
  even where both sides show null.
- **Prolly:** the value is shown null too, with one warning per column
  and version counting them (a key column's refuses the read, naming
  it). An old value is made the column's as an assignment would make
  it: the cast to the base type, then the typmod (a length coercion
  that fails rather than cuts, as an INSERT's does; a numeric rounds,
  as it does) and the domain's constraints. A value that fails any of
  them is one that doesn't convert (#590). A key column's value meets
  only the cast, not the typmod or domain (a narrowed varchar's key
  shows as stored), since no key shows null; and a merge's conflicts
  are listed without them too, so what's listed is what resolving
  writes (resolving refuses a value the column can't hold, as an INSERT
  does). The fitting is only what's shown: rows are compared as the
  cast reads them (a lossy cast between types stored alike keeps the
  stored value), so a row changed between two old values that both
  fail to fit (both shown null, or rounded alike) is modified, as in
  Dolt, and one an ALTER rounded is too. Rows are compared as the table
  is defined now, so a row whose only change is to a value the cast
  itself fails on (a column dropped, its old values not converting to
  the type it was added back as) isn't modified.
- **Why:** a diff reads each version as the table is defined now,
  through its casts (D20); what didn't convert has no value left to
  compare.
- **Port:** the drop-and-recreate case whose old values don't convert
  expects no rows for the drop's commit, and the count without them.

## D22. Across a primary key change, rows are read as keyed now

- **Dolt:** a row diff across a change of a table's primary key columns
  warns that it can't render the full diff and shows what it can
  (rows added and removed under the old and new column tags).
- **Prolly:** each version's rows are read as the table is keyed now,
  a keyless version's as keyed rows and a keyed one's as keyless, so
  they diff as any rows do: the key's change itself changes no row.
  Rows of a version that share a key as it is defined now are shown
  once, with a warning counting the rest. A merge across such a change
  is still refused. `dolt_diff_stat` doesn't read versions so: it
  counts rows only where the versions are keyed as the table is now,
  as a keyless table's copies and a keyed one's modified rows aren't
  the same counts.
- **Why:** a diff reads versions as the table is defined now (D20, D21),
  and the canonical form keys rows by their values, not column tags.
- **Port:** the key-change cases expect the rows as keyed now: fewer
  than Dolt's, and none for the key's own commit.

## D23. `dolt_gc()` returns a status, not statistics

- **Dolt:** `CALL dolt_gc()` returns a status row (`{0}`), and logs
  what it kept and freed.
- **Prolly:** `dolt_gc()` returns `{0}` too; what each collection kept
  and freed, and how long it held its window, is counted in
  `pg_stat_prolly` (`gc_collections`, `gc_chunks_kept`,
  `gc_chunks_freed`, `gc_bytes_freed`, `gc_window_usec`), and a
  collection's sizes are `prolly_store_sizes()`'s.
- **Why:** Dolt's return shape, kept; Postgres's statistics views are
  where work is counted.
- **Port:** a case reads the counters before and after a collection
  for what it did, not `dolt_gc()`'s result.

## D24. Extension values reconstruct through their binary receive function

- **Prolly:** a value stored through an extension's binary I/O comes back
  as `receive(send(value))` defines it. For example, PostGIS points with
  either positive or negative NaN coordinates reconstruct as `POINT EMPTY`
  after reset, clone, or materialization from a commit. The working heap
  may retain the original spelling until it is reconstructed.
- **Why:** the extension's wire format may discard physical distinctions.
  This follows PostgreSQL dump/restore and binary COPY behavior, without
  a PostGIS-specific lossless format. Format 17 stores normalized send
  bytes, so those spellings also have the same canonical content hash.
  Primary keys must compare equal before and after normalization under
  their operator class; unsafe keys are refused.
- **Port:** compare reconstructed values using the receiver's semantics.
  The generic normalizing-type regression covers hashes, cell merges and
  reset; the PostGIS-specific regression remains tracked in #689/#710.

## D25. Checking out a tag is a read-only detached session

- **Dolt:** `dolt_checkout('v1')` of a tag (or a commit) is refused: Dolt
  has no detached head in SQL, and suggests `dolt_checkout('v1', '-b',
  name)`. A tag is read-only as a revision database, `USE mydb/v1`.
- **Prolly:** a tag (or commit hash) that names no branch checks out as a
  read-only session on the commit's hidden branch, `detached/<hash>`,
  as a commit's hash already does; `-b` makes a branch there as in Dolt.
- **Why:** a connection names a branch (D11), so the session a revision
  database gives in Dolt is this checkout here; refusing it would leave
  no way to read a tag's state as a session. Proposed in #704, open for
  the user's review.
- **Port:** an assertion of Dolt's refusal is omitted (the case is
  partial); test_prolly's `tags` test checks the read-only checkout.

## D26. Reading a table's history takes its grants, over its lineage

- **Dolt:** a user who may read a database reads any table at any commit
  by hash, `AS OF` or a history table; privileges are on databases and
  tables by name, not on versions.
- **Prolly:** every history read of a table's rows (`dolt_at`,
  `dolt_history_table`, `dolt_diff`, `dolt_diff_table`, by row type or
  by name) takes `SELECT` on the table here of the version's name, and
  that grant covers only versions that are that table, as relation-made
  history: each commit this database makes records the relations its
  tables were, and a version is the table's when its commit was made from
  the table's relation, reached from the working set and the head by
  steps whose commits were both made from it (a rename followed), or
  along another branch's commits made from it. A version of another table
  of its name (one dropped, and a table made again with its name), and a
  commit without a record (made in another database: fetched, pulled,
  cloned or pushed), are read only by the database's owner or a member of
  `pg_read_all_data`. So a head moved to the dropped table's commit and
  then committed (a head move also takes D27's privileges), a squash of
  the drop into the re-make, a merge of a branch that still has the old
  table, or a branch made at the old table's commit, doesn't vouch for the
  old table's versions, nor does a table dropped and made again within
  one commit (its record names the new relation, its parent's the old).
  A merge, pick or revert committed over uncommitted changes is recorded
  made only from what its first parent's record and the working set both
  have. Known limits, as commits name tables by name and identity alone
  and a table made again keeps its name's identity (#891, decided: commit
  identities don't change): a branch's first use may take its copies from another branch's relation,
  whose commits it then makes; a move that brings the old table's rows (a
  merge, a hard reset, a revert of the drop, a cherry-pick) writes them
  into the new table, where they are its rows
  ([#936](https://github.com/coffeegoddd/postgres/issues/936)); a table a
  merge makes is a relation of its own, so its source branch's history
  isn't its grantee's (this and the unrecorded commits fail closed); and
  a role given a dropped relation's OID again would read that relation's
  versions. Planner estimates of a version's rows and `dolt_diff_stat`'s
  counts are history too: a version the user may not read gives the
  default estimate, and its table is left out of `dolt_diff_stat`
  (refused when named). Any commit is otherwise read by hash, as in Dolt.
- **Why:** Postgres grants are on relations; a table made again with an
  old name is another relation, and its grants don't cover the dropped
  one's rows. Commits name tables by name and identity alone, so the
  names can't tell a table made again from the one dropped (#891); each
  commit's record of the relations it was made from can.
- **Port:** ports run as a superuser; none is affected.

## D27. Moving a branch's head takes a write role, or the branch

- **Dolt:** branch permissions are rows of `dolt_branch_control`: a user
  and host pattern given `write` or `admin` on database and branch name
  patterns. Modifying a branch (a commit, merge, reset, deletion or
  rename) takes `write` on it, and a branch's creator is given `admin` on
  it. By default one row gives every user `write` on every branch, so
  branches are guarded by SQL privileges alone until the table is edited.
- **Prolly:** a branch's head moves (`dolt_commit`, the commit of
  `dolt.transaction_commit`, `dolt_reset` of any kind, `dolt_merge`,
  `dolt_pull`, `dolt_cherry_pick`, `dolt_revert`, `dolt_rebase`,
  `dolt_integrate`, and `dolt_branch`'s delete, rename and forced move)
  take one of: owning the branch (having made it; main, and a branch
  whose maker was dropped, is the database owner's; `dolt_branches.creator`
  shows it), owning the database, the database's `CREATE` privilege, or
  the privileges of the predefined role `pg_prolly_write`. Privileges on
  the branch's tables alone don't give them: a role with `SELECT` can't
  move a branch every other session on it reads, nor undo the move to
  hide it. Deleting a branch, the database owner's alone before, takes
  the same; a rename's new name takes `CREATE` or `pg_prolly_write`, as
  making a branch takes `CREATE`. Making a tag, and adding or removing a
  remote, take `CREATE` or `pg_prolly_write` (or owning the database);
  deleting a tag takes those or having made it. A stash is dropped or
  popped only by the role that pushed it or one with those privileges,
  and a clear by another role clears only its own. In the writer modes a
  writer with only table grants commits on its writer branch and
  integrates its own writer branch into the protected base when every
  change it brings is to rows of tables it may write as they changed
  (`INSERT`, `UPDATE`, `DELETE`, as each change needs); a change to a
  definition or to rows it may not write so, or another branch's
  integration, takes the privileges above. There is no per-branch table of grants:
  protected branches stay the database owner's to move outside
  integrations. A branch's owner is recorded by role OID, not as a
  dependency: `DROP ROLE` doesn't refuse it, `REASSIGN OWNED` and
  `DROP OWNED` don't move or drop it, and a role later given a dropped
  role's OID (after OID wraparound, or as a dump and restore assigns role
  OIDs anew) would own its branches, tags and stashes
  ([#931](https://github.com/coffeegoddd/postgres/issues/931)). A fetch
  sets the remote's tags here only for a role with those privileges, and
  a fetched or pushed tag has no maker here.
- **Why:** Postgres has no default write grant on a database to mirror
  Dolt's default row; its roles and `CREATE` (which already makes
  branches) are the native way to say who may change shared state, and
  the branch's maker owns it as a relation's creator owns it.
- **Port:** ports run as a superuser; none is affected. A script that
  commits as a role with table privileges alone grants it
  `pg_prolly_write` (or `CREATE` on the database).
