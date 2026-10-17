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
  --check ../corpus/dolt/source-lock.json --out /tmp/dolt-catalog
```

The command reads Git objects at the pin, ignoring working-tree edits.
`catalog.json` contains each case's path, suite, zero-based index, line, exact
literal bytes and SHA-256, setup/assertion counts, skip flags, and computed SQL
expressions requiring review. It never executes or translates upstream Go.
The smaller committed `source-lock.json` pins file hashes and counts; the full
catalog is a generated artifact.

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

The first tranche preserves all 23 assertion entries from three cases:

| Source behavior | Scenario |
| --- | --- |
| Three independent committed working sets | dolt-checkout-working-set |
| Create, leave and revisit a branch | dolt-checkout-new-branch |
| Distinct commits, HEAD/parent/main references and invalid refs | dolt-hashof |

These ports explicitly adapt MySQL CALL/staging flags to native commit-all
operations, capture returned commit IDs instead of MySQL variables, use
PostgreSQL identity columns, and check PostgreSQL SQLSTATEs. Upstream
`SkipResultsCheck` on checkout becomes operation success plus active-branch
verification; it is not propagated as a bypass.

The manifest also records four deferred cases with concrete reasons. One is
an intended divergence: a hard reset restores a versioned schema's
sequences to the commit's values (they are per branch and in each commit),
where Dolt continues its auto-increment counter.
An active-transaction reset, mixed reset/staging, and merge-state reset cannot
be represented by the current adapter without changing the behavior under
test. Another 1,226 catalog cases remain unreviewed. No subset pass is a claim
of full Dolt compatibility.

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
the committed lock and mappings before running the imported scenarios on D1.
The D1 execution and coverage commands must both exit successfully.

A separate contract check records the same cases as unsupported on heap,
prolly, D2, D3a, D3b and D4. Both commands must exit with the expected
incomplete-coverage status, and the generated ledger must contain exactly
those six engines, every selected case unsupported, and zero passes, failures,
not-run cases or credited assertions. This checks truthful capability
reporting; it does not test VC correctness on those engines. Enabling VC on
another engine requires moving it into the success gate and updating this
explicit ledger expectation.

The existing `test-logs` artifact includes the generated source catalog,
mapping review, saved scenarios and runner/server logs, plus separate D1 and
unsupported-engine coverage reports under `engine-results/dolt-*`.
