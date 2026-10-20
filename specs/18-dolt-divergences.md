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
  Historical enum values are translated using the saved type definitions
  into that current row type (#387). A pure rename preserves each label's
  position; surviving names must keep their positions. Additions preserve
  existing labels by name and order, so a type that only gained labels
  leaves its tables' rows reading as they did (no table changes with it). A label-only rename therefore produces
  no typed row diff; the enum's definition still changes. Actual row edits,
  additions, deletions and conflicts use the current labels on both sides.
  This includes domains, arrays, unchanged composites, enum keys and keyless
  copy counts. Mixed renames and additions, reordered/reused names, changes
  to enclosing type definitions, ranges holding renamed enums, and merges
  that also drop columns are refused with a schema-conflict error rather
  than guessed. History stores definitions, not the individual rename
  commands; there is no change to the canonical storage format.
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
  `dolt_diff` is the branch's working set as the current transaction
  sees it, its own uncommitted writes included, as a Dolt session's
  working root includes its own (#501).
- **Why:** Postgres's atomicity: a statement's effects are the
  transaction's.
- **Port:** a case asserting that reset committed the transaction is
  deferred with this ID (`ports.json` has one); add an explicit `COMMIT`
  only where the assertion isn't about that. Reads of `WORKING` after
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
- **Prolly:** commit ids are 40 hex digits of BLAKE3's first 20 bytes
  (64 hex digits of SHA-256 until spec 22's group A), canonical: the
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
  `detached/...`, a commit's hidden branch (spec 11), whether or not it
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
  new one), and the row not written is recorded alone, once, for the
  first unique constraint it breaks. Check and not-null constraints are
  the same: their breaking rows are recorded, not written.
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
- **Port:** expected rows and violations follow prolly's: the violation
  lists the row not written, and the table has one row fewer than
  Dolt's (a count of its rows, as `DELETE`'s, is one lower).

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
- **Why:** spec 21's decision (user, 2026-10-19): the other kinds are
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
- **Why:** Postgres's DDL, from what the commit holds (spec 21, 4.2);
  constraints are named objects in Postgres, so they are statements of
  their own.
- **Port:** expected statements are prolly's rendering of the case's
  tables; which rows appear, and which side is empty, are Dolt's.

## D17. Renames are matched by content

- **Dolt:** a table renamed is matched by its columns' tags, which
  survive renames, so a table renamed and changed in one commit (rows
  added, say) is one `renamed` row.
- **Prolly:** the canonical form has no tags. A dropped and an added
  object of one kind whose definitions (but for their own names) and
  data are equal, and the only such pair, are one `renamed` row
  (`data_change` false, `schema_change` true, as Dolt's); a rename that
  also changes rows or definition is a drop and an add. A name argument
  naming either side gives the one row, as in Dolt.
- **Why:** spec 09's rule, a rename is a removal and an addition of
  identical content (spec 21, 4.3); nothing else in a commit says two
  objects are one.
- **Port:** a case renaming a table and changing its rows in one commit
  expects a `dropped` and an `added` row; one asking by either name
  gets that name's row alone.

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
  `commit_date`) and `dolt_at(ref, NULL::t)` (`SETOF t`). The `dolt_diff`
  view, over `dolt_diff()`, is the system table, every kind of object
  listed (D15). Rows of other versions are decoded as the table is
  defined now, by column name (spec 21, decision 3), where Dolt follows
  column tags: a column dropped changes no row (the rows read the
  same); a column renamed reads as NULL in versions before the rename,
  so a row diff across it shows the row modified (or a stat counts the
  cell); a column of another type in an older version is converted to
  today's type (text `'3'` reads as integer 3, where Dolt gives NULL).
  A table's history is followed by its name: one dropped and made again
  has both tables' rows.
- **Why:** Postgres's catalogs; the functions keep a table's own row
  type. The canonical form records no column tags.
- **Port:** `SELECT * FROM dolt_diff_t` becomes `dolt_diff_table(NULL::t)`
  with `(to_row).col` for `to_col`; `AS OF 'r'` becomes `dolt_at('r',
  NULL::t)`; `dolt_history_t` becomes `dolt_history_table(NULL::t)` with
  `(row).col`. Expected rows follow the reading by name.

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
  is shown null with a warning per value; a row whose stored value
  changed is modified even where both sides show null.
- **Prolly:** the value is shown null too, with one warning per column
  and version counting them (a key column's refuses the read, naming
  it). Rows are compared as the table is defined now, so a row whose
  only change is to such a value (a column dropped, its old values not
  converting to the type it was added back as) isn't modified.
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
