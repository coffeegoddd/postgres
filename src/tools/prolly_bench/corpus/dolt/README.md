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
