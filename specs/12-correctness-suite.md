# 12. The C suite: SQL correctness against stock Postgres

**Decided by the user 2026-10-15,** with the choices at the end as
recommended. Status of the build is in the plan's table.

## Purpose

Run the sqllogictest corpus against stock Postgres (heap) and against each
of our engines with one command, and compare the engines' results with
heap's. It is the third suite beside S (storage) and V (version control)
in `06-benchmarking.md`, and runs from the same harness,
`src/tools/prolly_bench/`.

Dolt and Doltgres publish a pass percentage against the corpus's expected
answers (Doltgres 1.0.0: 99.3% of 5.68M). For us that percentage is
secondary. Our engines share Postgres's parser, planner and executor and
change only storage, so **any answer that differs from heap's is an engine
bug** unless it differs only in row order. The C suite therefore treats
agreement with heap as its main result, and the corpus's own expectations
as a second view that can be set beside Dolt's and Doltgres's numbers.

It complements the correctness gates in `06-benchmarking.md`: gate 1
(Postgres regress under the table AM) tests features; the C suite tests
volume, with millions of generated queries, many of them through indexes,
and tens of thousands of CREATE and DROP statements against the
branch-aware catalogs. It runs one session per worker and does no crash
testing, so it replaces neither the isolation gate nor the recovery gate.

## The corpus

`~/cursor_src/sqllogictest`, Dolthub's fork of SQLite's sqllogictest, at
`04b0d19` (2026-10-15), the commit that added Doltgres support. The Go
runner (`go/logictest`) is in the same repository; Doltgres pins it at
`788480b`, the same content.

| Group | Files | Records | What it exercises |
|---|---|---|---|
| `select1`–`select5` | 5 | 10.7k | Joins, subqueries, expressions over small tables |
| `evidence/` | 12 | 0.5k | Statements from SQLite's documentation: DDL, views, `IN`, aggregates |
| `index/` | 214 | 2.11M | The same queries with and without indexes: `between`, `commute`, `delete`, `in`, `orderby`, `orderby_nosort`, `random`, `view` |
| `random/` | 391 | 5.30M | Generated `aggregates`, `expr`, `groupby`, `select` |
| `ddl/` | 1 | 4.6k | CREATE and DROP of tables, indexes and views |

Across the corpus: about 12.4k `CREATE TABLE`, 17.3k `CREATE [UNIQUE]
INDEX`, 14.6k `CREATE VIEW`, 5.5k `DROP TABLE`, 116k `INSERT`, 20k
`DELETE`. Postgres is a supported target: 264.5k `skipif postgresql` lines
leave out what it can't run. 492k queries are `nosort`, meaning the
expected rows are in the order the engine returns them; roughly 25k of
those have no `ORDER BY` (an upper bound, counted on each query's first
line only). Those are the queries where an engine can disagree with heap
on order alone.

## Running it

```sh
# once, and whenever the corpus or the runner changes
src/tools/prolly_bench/build_slt.sh          # builds build-bench/slt/slt-runner

# baseline and engines; one result set per table AM, then the comparison
src/tools/prolly_bench/prolly_bench run C --am heap,prolly,prolly_d1

# or one at a time, and a subset
src/tools/prolly_bench/prolly_bench run C --am prolly --workloads C1,C2

# compare: counts per engine, and each engine against heap
src/tools/prolly_bench/prolly_bench table --suite C
src/tools/prolly_bench/prolly_bench cdiff specs/results/<date>-C-B0 \
                                          specs/results/<date>-C-prolly
```

`--am` takes a list for C. Heap runs first when present; the comparison is
written after the last engine. Results go to
`specs/results/<date>-C-<label>/` as for S and V, with heap labelled `B0`.

## Workloads

| ID | Files | Default config |
|---|---|---|
| C1 | `select1.test`–`select5.test` | smoke and standard |
| C2 | `evidence/` | smoke and standard |
| C3 | `ddl/` | smoke and standard |
| C4 | `index/` | standard; smoke takes one file per subdirectory |
| C5 | `random/` | standard |

`configs/c_smoke.toml` should take minutes and `configs/c_standard.toml`
runs the whole corpus. Step 1 of the plan measures both; the workers count
(default: half the CPUs) is set from that measurement. Per-record timeout
defaults to 60 s.

## Selecting the engine

Every database the runner creates gets `default_table_access_method` set to
the AM under test, so every table the corpus creates lands in it, as in the
S suite and `src/test/prolly` (`PROLLY_AM`).

**`prolly` needs a versioned schema.** Since the 2026-10-16 decision,
prolly tables exist only in versioned schemas, and with `prolly` as the
default, a table created anywhere else is quietly a heap table
(`versioned_relation_am` in `src/backend/catalog/versioned.c`). A run that
only set the default would test heap twice and report a perfect score. So
for `prolly` each database gets `CREATE SCHEMA slt VERSIONED` and
`search_path = slt`. The other AMs (`prolly_d1`–`prolly_d4`, from the
earlier design) need only the default.

The harness keeps this in one place: a target table mapping an AM to its
setup SQL (default AM, and for `prolly`, the versioned schema). The S and
V suites have the same problem today: `prolly_bench run S --am prolly`
measures heap. They should use the same target table (plan step 6).

**The AM is checked, not assumed.** Before the runner drops a file's
database, it lists the relations in the test schema with their AM. A table
or materialized view on any other AM fails the file with status `wrong_am`,
and a run with any `wrong_am` file exits non-zero. One check at the start
creates a table and confirms its AM before any file runs.

## The runner

The runner is a small Go program in `src/tools/prolly_bench/slt/` built on
`go/logictest`, adapted from Doltgres's (`testing/logictest/` in
`~/cursor_src/doltgresql`). The harness's Python stays standard library
only and drives it as it drives pgbench and BenchBase. A Python runner is
not an option: the standard library has no Postgres driver, and 7.4M
records through `psql` would be far too slow. The corpus's hashing and
sorting rules also have to match the reference runner exactly.

What it changes from Doltgres's harness:

| Doltgres | Ours | Why |
|---|---|---|
| Connection string hard-coded to port 5432, user `postgres`, password `password` | `-dsn` flag; the harness passes the cluster's address | `LocalCluster` uses a free port |
| Single-worker `Init` drops tables in schema `sqllogictest` | Every worker drops and recreates its database per file, then runs the target's setup SQL | On Postgres the tables are in `public`; the old cleanup leaves them behind |
| A `database/sql` pool per worker | One connection per worker (`SetMaxOpenConns(1)`) | The corpus assumes one session; session state such as `search_path` must not depend on which pooled connection runs a statement |
| Unknown column types are an error | Integer, float and numeric, boolean, and text types mapped by OID; anything else rendered as text and typed `T` | Avoids failures from the harness rather than the engine (`BPCHAR`, `DATE`) |
| Errors are message text | Error lines carry the SQLSTATE | `0A000` is `not_implemented`, as in S and V |
| No server-crash handling | See below | A crashing engine takes every worker's session down |

**Result digests.** For each executed query the runner also writes the
row count and two hashes of the actual result: in returned order, and with
rows sorted. Records are matched across engines by (file, line); the skip
directives depend only on the engine string (`postgresql`, the same for
every target), so every engine runs the same records. The digests are what
let the comparison tell order-only differences from wrong answers, and see
when an engine fails a record differently from heap.

**Crashes.** When a session's connection drops, the runner checks
`pg_postmaster_start_time()`. If the server restarted, the record that was
running in the crashing backend (the one whose log line the server log
names) is `crashed`, and the rest of its file is `did_not_run`. Files of
other workers that were running at the restart are run again once, from
the start. The server log around the crash is saved with the cell.

**Timeouts.** As in the reference runner, a timed-out record ends its
file: the remaining records are `did_not_run`. A timeout that heap doesn't
have is reported as a performance finding, not as a correctness failure.

## Results

The C suite uses the S suite's layout and adds per-record files:

| File | Contents |
|---|---|
| `results.jsonl` | One line per cell (workload): status, and counts per record status |
| `raw/<cell>/records.jsonl.gz` | One line per record: file, line, status, SQLSTATE, message, row count, both digests, milliseconds |
| `raw/<cell>/runner.log.gz` | The runner's own output (the reference format, readable by `logictest.ParseResultFile`) |
| `raw/<cell>/server.log.gz` | The server log |
| `environment.json` | As for S, plus the corpus commit and the runner's build commit |

Record statuses: `ok`, `not_ok`, `skipped`, `timeout`, `did_not_run`,
`not_implemented` (SQLSTATE `0A000`), `crashed`, `wrong_am` (per file).
Pass percentage is `ok` over records that weren't skipped, the figure
Doltgres publishes.

## Comparison

`prolly_bench table --suite C` gives a table of counts per engine, and for
each engine its comparison with B0. `cdiff` gives one engine's differences
from heap in full, grouped by file and by error, each with file and line,
the query, and both engines' answers (row counts and the first rows), so a
difference can be reproduced by hand.

Each record of an engine falls into one class:

| Heap | Engine | Class | Meaning |
|---|---|---|---|
| ok | ok | `agree` | |
| ok | not ok, same sorted digest | `order_only` | Same rows in a different order. Not a bug in itself; listed so it can be confirmed |
| ok | `not_implemented` | `not_implemented` | Expected for stubs |
| ok | not ok, other | `regression` | **The headline number.** Each one is an engine bug or a documented exception |
| not ok | not ok, same digests (or same SQLSTATE) | `postgres_semantics` | Postgres's own answer differs from the corpus; not the engine's doing |
| not ok | not ok, different | `hidden_regression` | Heap fails the record, but the engine answers differently again. Counted with regressions |
| not ok | ok | `engine_only_pass` | Should not happen with the same executor; investigate |
| any | `crashed`, `wrong_am` | as named | Always failures |

Timeouts and `did_not_run` are reported in their own columns, not
classified.

**Exceptions.** `src/tools/prolly_bench/configs/c_exceptions.toml` lists
records (file and line, per AM or for every AM) that are expected to
differ, each with a reason, as gate 1 documents its exceptions (tests that
print `ctid`). An exception that no longer differs is reported, so the list
doesn't go stale.

**Gate.** Proposed as gate 8 in `06-benchmarking.md`: an engine passes the
C suite when it has no `regression`, `hidden_regression`, `crashed` or
`wrong_am` records outside the exceptions. `order_only` and
`not_implemented` are reported but don't fail it.

## Plan

| Step | Work | Done when |
|---|---|---|
| 1 | `build_slt.sh` and the Go runner, with the changes above; run it by hand against a heap cluster | C1–C3 run on heap; timings for each group measured; the full corpus run once on heap |
| 2 | The C suite in `prolly_bench`: workloads C1–C5, suite `C` in the runner, `--am` list, the target table (default AM, versioned schema for `prolly`), the AM check, `c_smoke.toml` and `c_standard.toml` | `run C --am heap` writes a complete result set; a `wrong_am` canary fails as it should |
| 3 | Comparison: `table --suite C`, `cdiff`, the classes, the exceptions file | Classes computed from fixtures in `tests/test_units.py`; a run of heap against itself is all `agree` and `postgres_semantics` |
| 4 | Baseline: heap twice on the full corpus | The two runs agree record for record (the suite is deterministic on heap); heap's own failures sorted into `c_exceptions.toml` or left as `postgres_semantics` |
| 5 | The engines: `prolly`, then `prolly_d1` if still wanted | Their result sets and comparisons in `specs/results/`; regressions filed or fixed |
| 6 | The S and V suites use the target table | `run S --am prolly` creates its tables in a versioned schema; the AM check runs there too |
| 7 | Documentation: gate 8 in `06-benchmarking.md`, the C suite in the harness README, step status in `07-roadmap.md` | |

Code follows the repository's rule: no references to these specs from code
or comments.

## Decisions (user, 2026-10-15, as recommended)

1. **The gate is agreement with heap, not the corpus's expected answers.**
   Recommended: yes. The corpus's pass percentage is still reported.
2. **For `prolly`, run the corpus in a versioned schema.** Recommended: yes;
   it is the only way to put the corpus's tables on `prolly`. The cost is
   that the corpus then also tests versioned-schema rules (for example, no
   temporary tables in one; the corpus has two `CREATE TEMP` statements),
   which may produce exceptions that are rules, not bugs.
3. **Which engines.** Recommended: `heap` and `prolly`; `prolly_d1` (the
   earlier design, tagged `prolly-d1`) only if its results still matter.
   `prolly_d2`–`prolly_d4` are stubs and would report `not_implemented`
   throughout.
4. **A suite of `prolly_bench`, not a separate tool.** Recommended: yes; it
   reuses the cluster setup, `--pg-bin`, labels, environment recording and
   the results layout. The C suite has no timed window or collectors, so its
   cells skip both.
5. **CI.** Recommended: not yet. Once the baseline is clean, `c_smoke.toml`
   on `prolly` would fit the lean CI.
