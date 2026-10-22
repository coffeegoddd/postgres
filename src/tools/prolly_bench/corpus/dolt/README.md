# Dolt source catalog and reviewed ports

Upstream: https://github.com/dolthub/dolt at
`51cc4458b740ac132cb2302fe5394a2abbfba39d`.

The source lock covers top-level literal arrays of `queries.ScriptTest`,
`queries.TransactionTest`, and `MergeScriptTest` in
`go/libraries/doltcore/sqle/enginetest/*.go`. Its 1,233 cases and 5,820
literal assertion entries are a **source inventory, not executed coverage**.
Singletons, generated/appended cases, other Go test types, and Bats are outside
this denominator. Computed fields are reported as unresolved, not inferred.
The lock includes every Go file in that directory, even files with no matching
cases, so changes to the inventory's inputs are visible.

Reproduce it from a local Dolt clone (no Dolt build or network needed):

```sh
cd src/tools/prolly_bench/slt
go run ./cmd/dolt-catalog --repo ~/cursor_src/dolt \
  --revision 51cc4458b740ac132cb2302fe5394a2abbfba39d \
  --doltgres-repo ~/cursor_src/doltgresql \
  --doltgres-revision d5e05f46728e4a28116770eb683090c3ccd82ca4 \
  --check ../corpus/dolt/source-lock.json --out /tmp/dolt-catalog
```

The command reads Git objects at the pin, ignoring working-tree edits.
`catalog.json` contains each case's path, suite, zero-based index, line, exact
literal bytes and SHA-256, setup/assertion counts, skip flags, and computed SQL
expressions requiring review. It never executes or translates upstream Go.
The smaller committed `source-lock.json` pins file hashes and counts; the full
catalog is a generated artifact.

The lock also pins, by file hash only, the other sources ports derive from:
Dolt's Go tests that don't run its engine (`go_tests`: the merge truth
tables, keyless and schema merges, merge bases, revision specs and branch
names), at the same revision, and Doltgres's Postgres-dialect version-control
tests (`doltgres`, at `d5e05f46`). Those ports are SQL regression tests, not
scenarios; the pin makes an upstream change to their source visible. They
are in `src/test/modules/test_prolly/sql`: `merge_truth.sql` (the Go tests),
and `doltgres_vc.sql`, `doltgres_objects.sql` and `doltgres_fks_auth.sql`
(Doltgres's), each saying which upstream file and script it follows and
where prolly's answer differs.

## Classification and coverage

`classification.json` classifies every catalog case, with a reason:

- **P**: version control prolly supports, by area (merge, schema-merge,
  cherry-pick, revert, branch, checkout, reset, multisession, ...). Ported,
  or to be. Multi-session cases encode Dolt's optimistic model and are
  rederived under Postgres semantics rather than ported literally.
- **F**: needs a feature prolly lacks, named (diff-tables, history, as-of,
  preview-merge-conflicts, ...). Ported with the feature; where only one
  assertion needs it, a partial port is allowed with a review note.
- **N**: doesn't apply: MySQL-only syntax, types or variables, the staging
  area, Dolt internals, or suites that aren't about version control.

It was drafted by rules on each suite and on what each case's literal uses
(the first matching feature decides), then reviewed; a case's class changes
by editing its entry. The verifier checks that every catalog case is
classified once and that a full port is of a P case (a partial one of a P
or F case); with `--catalog`, as CI runs it, each entry's hash is checked
against its case's literal too (offline, only the ported and deferred
cases' hashes are). `dolt-corpus` reports P cases by area as
ported, partial, deferred or not started, and F cases by feature; `--markdown`
writes those tables, which CI adds to the job summary.

Upstream source and derived scenarios retain Dolthub's Apache-2.0 license;
see LICENSE. This catalog does not confer test coverage by itself.

## Reviewed executable ports

`ports.json` lists each selected source by path, suite, array index, name and
literal SHA-256, plus the reviewed scenario SHA-256. Exact upstream literals
are retained in `sources/`; modified PostgreSQL scenarios live in `scenarios/`.
See NOTICE for attribution. Every setup statement and assertion has a
zero-based mapping to scenario step IDs and a review note. An empty mapping
requires a note and makes the case partial, never fully ported. All scenario
steps must appear in the mapping.

A case whose `SetUpScript` names a shared script (`createConflictsSetupScript`,
say: a top-level `[]string` literal several cases use) has that field
unresolved in the catalog. The catalog also inventories each such script
(`setups` in `catalog.json`: its file, name, statement count and exact
literal bytes), and `ports.json`'s `setups` retain each one once, by ID, in
`sources/<id>.go.txt`. A port of the case names its script as
`shared_setup` and maps the script's statements as its setup, every one, as
it maps a literal setup's. The verifier checks that the script is the one the
case names (the only unresolved field allowed), from the same package, that
its excerpt is a literal of the recorded hash with no computed statements,
and, with `--catalog`, that the pinned file's literal of that name has the
same hash. A script no port uses is an error. Setups built by expressions
(`append(createConflictsSetupScript, ...)`) stay unresolved; their cases
aren't ported this way. A named script that is computed, or that no scanned
file declares at top level, is listed in the catalog's `unresolved_setups`
with the reason, and a port naming it fails with that reason. Without
`--catalog`, nothing ties a retained excerpt's bytes to the name recorded
for it; the catalog is what checks that, so CI always verifies with it.

Ports adapt MySQL `CALL` and staging flags to prolly's commit-all
operations (there is no staging area: `dolt_add` stages nothing), call
`dolt_*` functions with `SELECT`, capture returned commit IDs instead of
MySQL variables, use PostgreSQL DDL, and check PostgreSQL SQLSTATEs instead
of Dolt's error text. Each adaptation that follows from an intended
divergence from Dolt cites its stable ID in [the compatibility catalog](divergences.md)
(D1 no staging area,
D3 return shapes, D4 SQLSTATEs, D5 transactions, D6 merges and sequences,
D11 branch-qualified databases, ...). Dolt's `USE mydb/branch` becomes the
scenario's `connect` operation, and a reference to another branch's table
the same query on a second session connected to that branch.
Upstream `SkipResultsCheck` becomes operation success plus a check of the
resulting state (on checkout, the active branch); it is not propagated as a
bypass.

The manifest also records deferred cases, each with a concrete reason: an
intended divergence (for example, a hard reset restores a versioned
schema's sequences to the commit's values, since they are per branch and in
each commit, where Dolt continues its auto-increment counter), a case that
doesn't apply (a mixed reset needs the staging area), or a prolly behavior
still to be fixed. The remaining catalog cases are unreviewed. No subset
pass is a claim of full Dolt compatibility.

Validate the mappings (omit `--catalog` for an offline excerpt check):

```sh
cd src/tools/prolly_bench/slt
go run ./cmd/dolt-corpus --root ../corpus/dolt \
  --catalog /tmp/dolt-catalog/catalog.json --out /tmp/dolt-port-review.json
```

Run the selected scenarios and join the execution evidence:

```sh
bash src/tools/prolly_bench/build_vc.sh
src/tools/prolly_bench/prolly_bench vc --pg-bin /path/to/pgsql/bin \
  --cases src/tools/prolly_bench/corpus/dolt/scenarios --out /tmp/dolt-prolly
cd src/tools/prolly_bench/slt
go run ./cmd/dolt-corpus --root ../corpus/dolt \
  --results /tmp/dolt-prolly/results/summary.json --out /tmp/dolt-coverage.json
```

The report separates source inventory, fully/partially ported cases, deferred
and unreviewed cases, and execution outcomes per engine. Passed mapped
assertions are credited only for whole passing cases with the reviewed input
hash and complete step trace. Unsupported, failed and not-run cases receive
no assertion credit; missing/duplicate records or truncated passing traces
are errors. A report containing incomplete executions is saved, then the CLI
exits nonzero. Native adapter acceptance fixtures do not enter these counts.

To add a port, review the pinned literal and its setup dependencies, retain
its exact excerpt, implement the scenario, and map **every** source entry.
Record each dialect or semantic adaptation. Preserve expected rows, errors
and ordering; do not weaken assertions to fit current engine behavior.
Update the scenario hash only after reviewing the mapping. A new upstream
pin requires regenerating and reviewing the source lock and all affected
excerpts/mappings. The tools deliberately do not perform generic SQL
translation or execute arbitrary upstream Go.

## CI

Each PR fetches the pinned Dolt source, regenerates the catalog and verifies
the committed lock and mappings before running the imported scenarios on
prolly. The prolly execution and coverage commands must both exit
successfully.

A separate contract check records the same cases as unsupported on heap.
Both commands must exit with the expected incomplete-coverage status, and
the generated ledger must contain exactly that engine, every selected case
unsupported, and zero passes, failures, not-run cases or credited
assertions. This checks truthful capability reporting; it does not test VC
correctness on heap.

The existing `test-logs` artifact includes the generated source catalog,
mapping review, saved scenarios and runner/server logs, plus separate prolly
and unsupported-engine coverage reports under `engine-results/dolt-*`.
