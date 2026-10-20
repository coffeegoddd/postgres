# 21. Schema diffs

**Status: proposed 2026-10-19, on `1c9d57affc`; decided by the user the
same day, as recommended (part 7). Built 2026-10-19 as stack #520
(#514–#519 and the layers after them), not merged; part 8 says what each
built and where it departs. Tracked in #439; the rename bug it touches
is #437.**
A versioned schema's definitions are committed, branched, merged and
moved (spec 11, steps 4–5), but nothing in SQL shows what changed in
them: `dolt_status` says an object is `modified`, and `dolt_diff` only
returns rows. This proposes Dolt's two schema-diff table functions,
`dolt_diff_summary` and `dolt_schema_diff`, built from the canonical
descriptions the root already holds, and says what comes later.

## 1. What happens today

Measured on master at `1c9d57affc` (cassert build), a versioned schema
`app` with table `app.t`, view `app.vw` and function `app.f()`, each
change made after a commit:

| Change | `dolt_status()` | `dolt_diff('HEAD', 'WORKING', NULL::app.t)` |
|---|---|---|
| `ADD COLUMN` (with or without a default) | `app.t modified` | 0 rows |
| `ALTER COLUMN … TYPE`, `DROP COLUMN` | `app.t modified` | 0 rows |
| An index or CHECK constraint added | `app.t modified` | 0 rows |
| `CREATE OR REPLACE VIEW`, `… FUNCTION` | `app.vw modified`, `app.f() modified` | (not a table) |
| `RENAME COLUMN x TO y`, no row changed | `app.t modified` | every row `modified`, `(1,)` → `(1,7)` (#437) |

Between two commits, `dolt_diff('HEAD~1', 'HEAD', …)` behaves the same.
No `dolt_*` function reads definitions: of those in `pg_proc.dat` and
the views over them in `system_views.sql`, the only one about schemas is
`dolt_status`.

Spec 11's Dolt test-parity program (part 4) and spec 18 deferred "diff
system tables" as a large feature, to be issues of their own. None was
filed until #439. The pinned Dolt catalog
(`src/tools/prolly_bench/corpus/dolt/classification.json`) marks every
case of Dolt's four suites here F, feature `diff-tables`:
`DiffSummaryTableFunctionScriptTests` (17),
`SchemaDiffTableFunctionScriptTests` (2), `DiffStatTableFunctionScriptTests`
(15) and `UnscopedDiffSystemTableScriptTests` (8).

## 2. What Dolt and Doltgres do

Dolt (`~/cursor_src/dolt`,
`go/libraries/doltcore/sqle/dtablefunctions`):

- **`dolt_diff_summary(from, to [, table])`** (`dolt_diff_summary.go`):
  `from_table_name`, `to_table_name`, `diff_type`, `data_change`,
  `schema_change`. One row per table that differs. `diff_type` is one of
  `added`, `dropped`, `modified`, `renamed` (`diff/table_deltas.go`).
- **`dolt_schema_diff(from, to [, table])`** (`dolt_schema_diff.go`):
  `from_table_name`, `to_table_name`, `from_create_statement`,
  `to_create_statement`. One row per table whose schema differs; a data
  change alone gives no row.
- **`dolt_diff_stat(from, to [, table])`** (`dolt_diff_stat.go`): row
  and cell counts per table.
- **The `dolt_diff` system table** (`dtables/unscoped_diff_table.go`):
  one row per commit and table changed, with the commit's metadata,
  `data_change` and `schema_change`.

Dolt matches a renamed table by its column tags, which survive renames,
and reports it as `renamed` with both names.

Doltgres (`~/cursor_src/doltgresql`, `testing/go/dolt_functions_test.go`)
has the same columns. It names tables qualified (`public.t1`), uses an
empty string, not NULL, for the side a table is missing from, and prints
a Postgres `CREATE TABLE`:

```
{"", "public.t1", "", "CREATE TABLE \"t1\" (\n  \"pk\" integer NOT NULL,\n  PRIMARY KEY (\"pk\")\n);"}
```

## 3. What we have to build it from

- **Descriptions.** Every object a commit holds is an item of the root,
  keyed by kind and qualified name, its value its canonical description
  (`canon_folded.c`, the header comment): `t:` tables (data tree hash,
  then schema), `v:`, `m:`, `f:`, `y:`, `s:`, `c:`, `x:`, `n:`.
- **Data and schema told apart.** `folded_tables()`
  (`diffmerge_folded.c`) already compares a table's data tree hash and
  its description separately (`same_hash`, `same_description`), and
  `folded_objects()` (`canon_folded.c`) compares the other kinds'
  definitions (a sequence's value apart from its definition). So
  `data_change` and `schema_change` are comparisons already made.
- **DDL from descriptions.** `move_schema()` turns descriptions into
  statements: `move_table()` builds `CREATE TABLE` (and `PARTITION OF`),
  its constraints, indexes, triggers, rules, policies and statistics,
  and there are builders for `CREATE TYPE` (enum, composite, range),
  `CREATE DOMAIN`, `CREATE COLLATION`, `CREATE CAST` and aggregates.
  They collect statements in a `SchemaMove` that `run_statements()`
  executes afterwards. Views and functions are stored as their
  definitions.
- **Names.** `dolt_status` already names every kind as Postgres
  describes it (`app.f()`, a schema as `new schema`).

## 4. Design

### 4.1 `dolt_diff_summary(from text, to text [, name text])`

- Columns as Dolt's: `from_table_name text`, `to_table_name text`,
  `diff_type text`, `data_change bool`, `schema_change bool`.
- **Every object kind a commit holds**, not tables alone, named as
  `dolt_status` names them; one row per object that differs, sorted by
  name. Dolt has only tables. Our other kinds are versioned too, and
  leaving them out would hide changes `dolt_status` shows.
- `diff_type`: `added`, `dropped`, `modified`, `renamed` (4.3). For a
  missing side, `''`, as Doltgres does.
- `data_change`: a table's or materialized view's data tree hash
  differs, or a sequence's value. `false` for kinds without data.
- `schema_change`: the description (definition) differs. Added and
  dropped objects have both flags `true` if they hold data, otherwise
  `schema_change` alone: a table or materialized view holds data when
  it has rows (an empty table added is `false`, `true`, as in Dolt's
  `HasDataChanged`), a sequence always (its value).
- `WORKING` and `STAGED` read as `dolt_diff` reads them. Objects a read
  leaves out are listed as `dolt_status` lists them: `diff_type` `not
  covered`, both flags NULL.
- `name` restricts the result to one object, qualified or found on the
  search path. Unlike `dolt_diff`'s `NULL::t`, the object need not exist
  now: a name a version had is enough.

### 4.2 `dolt_schema_diff(from text, to text [, name text])`

- Columns as Dolt's: `from_table_name`, `to_table_name`,
  `from_create_statement`, `to_create_statement`.
- One row per object whose **definition** differs (`schema_change`);
  a change of rows alone gives none, as in Dolt.
- Each statement is rendered from that version's description, never from
  the catalogs, so any two refs can be compared whatever the session's
  branch holds. A table is its `CREATE TABLE` followed by its
  constraints, indexes, triggers, rules, policies and statistics, each a
  statement ending in `;`, one per line as `move_table()` builds them.
  Other kinds: the statement that makes them (`CREATE VIEW …`, the
  function's definition, `CREATE TYPE …`). Names qualified.
- **Building it:** the statement builders in `move_schema()` are split
  from the move, so a description renders to text with no catalog reads
  and nothing executed. The move keeps using them. This is the bulk of
  the work.
- A root of an older `"!:format"` is refused, with the error moves give
  ("commit the branch" to write it anew), as its descriptions aren't
  read (spec 11, step 4, "As built").

### 4.3 Renames

Spec 09 says a rename is a removal and an addition with identical
content, which a diff reports as a rename by matching content hashes.
Question 35's merge rule (user, 2026-10-14) treats it as a drop and an
add. Proposed for diffs:

- **Objects.** A dropped and an added object of the same kind whose
  description and data tree hash are equal are reported as one
  `renamed` row, both names given. Matched in a hash table, O(u + v)
  for u dropped and v added (spec 10, status row). Only one-to-one
  matches count; several equal candidates stay dropped and added. Its
  flags are Dolt's for a rename: `data_change` `false` (the rows are
  equal), `schema_change` `true` (the name is part of the definition;
  `dolt_schema_diff` gives the row too). A rename that also changes rows
  or definition is a drop and an add, where Dolt, matching by column
  tags, reports `renamed` (spec 18, D17).
- **Columns.** The canonical form doesn't record dropped columns, so a
  rename and a drop plus an add at the end are the same description; the
  schema diff shows both statements and doesn't call it a rename.
- **Rows (#437).** When the from and to data trees have the same hash,
  `dolt_diff` returns no rows for the table, whatever the descriptions
  say: rows are encoded by position, so the rows didn't change. That
  fixes a rename with no row changes now. Reading columns by position
  in general is part 7's question 3.

### 4.4 Cost

`folded_tables()` and `folded_objects()` walk both roots' items in full,
O(T) for T items, which spec 10's status row allows (O(M), M = O(T + d
log d)) but doesn't aim at. Both functions here should walk the tree
diff of the two roots instead, which skips equal subtrees: O((d + 1) log
T) for d changed objects, spec 10's aim, plus the rename match.
Rendering statements costs O(size of the d descriptions). Neither
function reads rows, so a summary costs no row reads either: the data
tree hash answers `data_change`.

### 4.5 Privileges

Definitions are readable in Postgres's catalogs by anyone who can
connect (`pg_get_viewdef`, `pg_proc.prosrc`, `pg_policies`), so neither
function checks privileges on the objects, as `dolt_status` doesn't.
History can hold objects since dropped; their definitions are the
database's history, which `dolt_log` already shows anyone.

## 5. Later, separately

- `dolt_diff_stat`: row and cell counts. Rows need a tree diff per
  table, cells a decode; its own issue.
- The `dolt_diff` system view over history, with `data_change` and
  `schema_change` per commit and object: a commit walk of `dolt_log`'s
  shape, each commit diffed against its first parent with 4.1's
  summary.
- `dolt_diff_<table>`, `dolt_history_<table>`, `AS OF`: as spec 11's
  parity program lists them.

## 6. Tests

- `src/test/modules/test_prolly/sql/schema_diffs.sql` (new): every kind
  added, dropped, modified and renamed; data-only and schema-only
  changes; `WORKING` with uncovered objects; a name only an old version
  has; an older-format root refused.
- `status_diff.sql`: #437's cases.
- Ports: Dolt's `DiffSummaryTableFunctionScriptTests` and
  `SchemaDiffTableFunctionScriptTests` and Doltgres's `dolt_schema_diff`
  cases (`dolt_functions_test.go`) move from F to ported in
  `classification.json` and `ports.json`, each difference cited from
  spec 18.
- Spec 18 gains entries: other kinds than tables in both functions;
  Postgres DDL in statements; renames matched by content, not tags.

**As ported:** of the 19 Dolt cases, 11 summary cases are ported whole
(now P, area `diff-tables`), and 5 in part: `WORKING and STAGED` (its
STAGED-versus-WORKING assertions are about the staging area, D1), two
summary cases left F on `diff-ranges` (Dolt's `a..b` and `a...b`
arguments, which prolly lacks), `basic schema changes` F on `tags`
(and ranges), and `prepared table functions` F on `diff-tables`
(`dolt_diff_stat`). Three are deferred: the two primary-key-change
cases (Dolt refuses or skips such a table; prolly lists it as modified,
to be decided, D15) and the drop-and-recreate case, whose setup is
computed and is checked in `schema_diffs` instead. Spec 18's D15–D17
are the entries. Porting found two differences from Dolt, fixed to
match it: an empty table added or dropped had `data_change` true, and
a renamed object `schema_change` false. Doltgres's cases are in
`schema_diffs`.

**Ported with the later functions** (ranges, `dolt_diff_stat`, the
per-table functions, the view): the range assertions are ported, so
`diff with branch refs`, `renamed table` and `prepared table
functions` are whole (P); the primary-key-change cases are ported with
D19 (of the 17 summary cases, 15 whole, `WORKING and STAGED` in part,
drop-and-recreate deferred). Of `DiffStatTableFunctionScriptTests`, 13
cases are ported, 11 whole and 2 in part (staging, as above; and
`basic case with single table`, whose table is dropped before the
assertions: the stat counts such a table's rows from the commits' trees
(#528) but not its cells modified, which need its rows decoded, #537),
and the keyless one (for that reason) and the computed one are
deferred. Of `DiffSystemTableScriptTests`, 17
(the `dolt_diff_<t>` cases and one of the view; one without its SHOW
INDEX), and the case whose column's old values don't convert to its
new type in part (D21: they show null, with a warning, #531), and the
two primary-key changes, read as the table is keyed now (D22, #530):
one whole, one in part (its warning left out). Of `HistorySystemTableScriptTests`, cases
0–8 (two without their EXPLAIN assertions); of
`UnscopedDiffSystemTableScriptTests`, cases 1–5. Differences follow
D15–D20; D18 says how rows of other versions are read.

## 7. Decisions

Decided by the user 2026-10-19, as recommended:

1. **Scope:** both functions cover every object kind a commit holds, not
   tables alone as Dolt's do (a divergence for spec 18).
2. **Missing side:** `''`, as Doltgres, so ported tests hold.
3. **Renamed columns in row diffs:** read by name, as today, with #437's
   rule: a table whose data trees have the same hash has no row changes.
   Reading by position is rejected, since the canonical form can't tell
   a rename from a drop and an add.
4. **No `alter` column:** the statements moving one version to the other
   depend on the session's catalogs; not now.
5. **Order:** before spec 11's step 8. #437 first, then 4.1, then 4.2;
   5's items later, each its own issue.

## 8. As built (2026-10-19)

- **#437 first** (#514): a table whose data trees have the same hash has
  no row changes in `dolt_diff`, whatever its descriptions say.
- **4.1, `dolt_diff_summary`** (#515): as designed. The canonicalizer's
  `changes()` reads the roots' tree diff (4.4's cost); the diff-merge
  component's `summary()` resolves refs. Renames compare items with
  each object's own name masked, as a table's description names it in
  its indexes' statements. From the ports (#517), as Dolt: an empty
  table added or dropped has no `data_change`, and a renamed object has
  `schema_change`.
- **4.2, `dolt_schema_diff`** (#516): rendered by the move's builders
  (`move_table()` rendering: each constraint whole, nothing read from the
  catalogs) through the canonicalizer's `render()`. A row is given where
  the two sides' statements differ, so a rename has one, as in Dolt.
- **Part 6's ports** (#517): of Dolt's 19 cases, 11 ported whole, 5 in
  part, 3 deferred (a primary-key change, D19; a computed setup); spec
  18's D15–D17; Doltgres's cases in `schema_diffs`.
- **Part 5, built too:**
  - `dolt_diff_stat` (#518, #496), on the summary and the row diff
    rather than a new interface callback;
  - `dolt_diff_table(NULL::t [, from, to])` and the `dolt_diff` view
    (#519, #497), D18's shapes;
  - `dolt_at(ref, NULL::t)` and `dolt_history_table(NULL::t)` (#495's
    SRF, #497's history), read from commits' trees.
- **A name argument** is read as Postgres reads identifiers, case folded
  and quotes kept (#522). One naming a table neither version has makes
  `dolt_diff_stat` refuse (42P01), as Dolt's does; `dolt_diff_summary`
  gives no rows for it, as Dolt's does too (#529).
- **A table the branch doesn't have** has its rows counted by
  `dolt_diff_stat` from the commits' trees (#528): the tree interface's
  `diff_unordered` diffs them by node hashes and key bytes, as no
  relation orders their keys. Its cells modified, and a keyless one's
  counts, need its rows decoded: still NULL (#537).
- **Ref ranges** (#521), as Dolt takes them: `dolt_diff_summary`,
  `dolt_schema_diff` and `dolt_diff_stat` take `'a..b'` (a to b) or
  `'a...b'` (from their merge base to b) in place of two refs, the next
  argument then the name. `dolt_diff`'s row type argument comes third, so
  it keeps two refs.
- **Not built:** `dolt_at`'s key-range pushdown and the detached checkout
  cache (#495).

