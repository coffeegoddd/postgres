# 12. The C suite: SQL correctness against stock Postgres

**Decided by the user 2026-10-15,** with the choices at the end as
recommended. **Built 2026-10-15**; where the build departed from the first
draft, the sections below describe what was built, and the plan's table
has each step's result.

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
| `ddl/` | 1 | 4.6k | MySQL `CREATE TABLE` options; every record is `onlyif mysql`, so it is left out |

Across the corpus: about 12.4k `CREATE TABLE`, 17.3k `CREATE [UNIQUE]
INDEX`, 14.6k `CREATE VIEW`, 5.5k `DROP TABLE`, 116k `INSERT`, 20k
`DELETE`. Postgres is a supported target: 264.5k `skipif postgresql` lines
leave out what it can't run. 492k queries are `nosort`, meaning the
expected rows are in the order the engine returns them; roughly 25k of
those have no `ORDER BY` (an upper bound, counted on each query's first
line only). Those are the queries where an engine can disagree with heap
on order alone.

Of the 7.4M records, 5,675,180 run on Postgres (the rest are skipped by
their directives), the same total Doltgres publishes.

## Running it

```sh
# once, and whenever the corpus or the runner changes
src/tools/prolly_bench/build_slt.sh          # builds build-bench/slt/slt-runner

# baseline and engines; one result set per table AM, then the comparison
src/tools/prolly_bench/prolly_bench run C --am heap,prolly

# or one at a time, and a subset
src/tools/prolly_bench/prolly_bench run C --am prolly --workloads C1,C2

# compare: counts per engine, and each engine against heap
src/tools/prolly_bench/prolly_bench table --suite C
src/tools/prolly_bench/prolly_bench cdiff specs/results/<date>-C-B0 \
                                          specs/results/<date>-C-prolly
```

`--am` takes a list. Heap runs first when present; the comparison is
printed after the last engine. Results go to
`specs/results/<date>-C-<label>/` as for S and V, with heap labelled `B0`.

## Workloads

| ID | Files | Default config |
|---|---|---|
| C1 | `select1.test`–`select5.test` | smoke and standard |
| C2 | `evidence/` | smoke and standard |
| C3 | `index/` | standard; smoke takes the first file of each directory |
| C4 | `random/` | standard; smoke takes the first file of each directory |

`configs/c_smoke.toml` takes about 50 seconds per AM and
`configs/c_standard.toml`, the whole corpus, a little over two minutes
per AM on the development machine (16 CPUs; the four cells take 109 s on
heap and 124 s on `prolly`). Workers default to one per CPU; the
per-record timeout is 30 s.

**What makes it fast.** A naive run, one connection through the corpus,
takes hours. Measured on heap, the whole corpus in one runner call takes
under two minutes:

- One child process per test file, from a pool of workers, largest files
  first: each worker takes the next file when it finishes one, as the
  correctness job in `~/cursor_src/ld/go/correctness` does, so the run
  isn't bound by an unlucky static split.
- C configs turn off `fsync`, `synchronous_commit` and `full_page_writes`:
  the corpus commits after each of its ~116k INSERTs and ~50k DDL
  statements, and the suite never crashes the server on purpose.
  Autovacuum is off too, so plans don't change during a run.
- A Unix socket, and the simple query protocol: one round trip per query.
  Doltgres's harness goes through `database/sql`, which prepares each
  distinct query, a second round trip across millions of unique queries.
- The per-file outputs are merged in parallel (3 s for the whole corpus,
  against 23 s in one thread).

The longest file, `select5.test` (many-table joins, about 100 ms each),
takes 34 s on its own and sets C1's time; it can't be split, since a file
is one session.

## Selecting the engine

Every database the runner creates gets `default_table_access_method` set to
the AM under test, so every table the corpus creates lands in it, as in the
S suite and `src/test/prolly` (`PROLLY_AM`).

**`prolly` needs a versioned schema.** Since the 2026-10-16 decision,
prolly tables exist only in versioned schemas, and with `prolly` as the
default, a table created anywhere else is quietly a heap table
(`versioned_relation_am` in `src/backend/catalog/versioned.c`). A run that
only set the default would test heap twice and report a perfect score. So
for `prolly` each database gets `CREATE SCHEMA versioned VERSIONED` and
`search_path = versioned`. (The earlier design's AMs, `prolly_d1`–`prolly_d4`,
needed only the default; they are removed by #183.)

The harness keeps this in one place, `pbench/core/targets.py`: for
`prolly`, the versioned schema. The S and V suites had the same problem:
`prolly_bench run S --am prolly` measured heap. They now use the same
target (plan step 6): each benchmark database gets the versioned schema
first on its `search_path`, and each cell checks its tables' AM after
loading.

**The AM is checked, not assumed.** After each file, the runner lists the
tables and materialized views left in the test schema with their AM; one
on any other AM is recorded against the file, and fails the cell. Before
any file runs, a canary table is created the way the tests create theirs
and its AM checked.

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
| Single-worker `Init` drops tables in schema `sqllogictest` | Each file gets a fresh database (dropped and created again for the worker slot), then the target's setup | On Postgres the tables are in `public`; the old cleanup leaves them behind |
| A `database/sql` pool, extended protocol | One `pgconn` session per file, simple protocol | The corpus assumes one session, and one round trip per query is faster |
| Unknown column types are an error | Integer, float and numeric, boolean, and text types mapped by OID; anything else rendered as text and typed `T` | Avoids failures from the harness rather than the engine (`BPCHAR`, `DATE`) |
| Errors are message text | Error lines carry the SQLSTATE | `0A000` is `not_implemented`, as in S and V |
| No server-crash handling | See below | A crashing engine takes every worker's session down |

**Result digests.** For each executed query the runner also writes the
row count and two hashes of the actual result: in returned order, and with
rows sorted. Only the sorted hash is used to compare answers: an
unordered result's order isn't stable between runs even on heap (two heap
runs of the whole corpus differed in returned order on 61 records, all
`rowsort` or `valuesort` queries without `ORDER BY`, with the same rows). Records are matched across engines by (file, line); the skip
directives depend only on the engine string (`postgresql`, the same for
every target), so every engine runs the same records. The digests are what
let the comparison tell order-only differences from wrong answers, and see
when an engine fails a record differently from heap.

**Crashes.** When a session's connection drops, the runner waits for the
server and checks `pg_postmaster_start_time()`. The record that was
running is `crashed`, and the rest of its file `did_not_run`. A file whose
backend the server log doesn't name as the one that died lost its session
to another backend's crash: it runs again, once, from the start, after the
others. The server log is kept with the cell.

**Timeouts.** As in the reference runner, a timed-out record ends its
file: the remaining records are `did_not_run`. A timeout that heap doesn't
have is reported as a performance finding, not as a correctness failure.

## Results

The C suite uses the S suite's layout and adds per-record files:

| File | Contents |
|---|---|
| `results.jsonl` | One line per cell (workload): status, and counts per record status |
| `raw/<cell>/slt/records.jsonl.gz` | One line per record that wasn't skipped: file, line, status, SQLSTATE, message, row count, both digests, milliseconds (about 80 MB for the corpus) |
| `raw/<cell>/slt/failures.log.gz` | The reference runner's log lines for records that didn't pass |
| `raw/<cell>/slt/files.jsonl` | One line per file: tables left, any on the wrong AM, time |
| `raw/<cell>/slt/summary.json` | Counts by status |
| `raw/<cell>/server.log.gz` | The server log |
| `compare/<baseline>/<cell>/` | In an engine's result set: `compare.json` (counts by class) and `diffs.jsonl.gz` against that baseline |
| `environment.json` | As for S, plus the corpus's commit and a hash of the runner's source (a stale build is a warning) |

Record statuses: `ok`, `not_ok`, `skipped`, `timeout`, `did_not_run`,
`not_implemented` (SQLSTATE `0A000` with "does not implement" or a
trailing "yet", the engines' wording; Postgres's own `0A000` errors stay
`not_ok`), `crashed`. Pass percentage is `ok` over records that weren't
skipped, the figure Doltgres publishes.

## Comparison

`prolly_bench table --suite C` gives a table of counts per engine, and for
each engine its comparison with B0. `cdiff` gives one engine's differences
from heap in full, grouped by class and by error, each with file and line,
the query (read from the corpus), and both sides' status, SQLSTATE, row
count and message. The rows themselves aren't kept; a difference is
reproduced by running the query by hand.

Each record of an engine falls into one class:

| Heap | Engine | Class | Meaning |
|---|---|---|---|
| ok | ok | `agree` | |
| ok | not ok, same sorted digest and row count | `order_only` | Same rows in a different order. Not a bug in itself; listed so it can be confirmed |
| ok | `not_implemented` | `not_implemented` | Expected for stubs |
| ok | not ok, other | `regression` | **The headline number.** Each one is an engine bug or a documented exception |
| not ok | not ok, same sorted digest (or same SQLSTATE, or same message) | `postgres_semantics` | Postgres's own answer differs from the corpus; not the engine's doing |
| not ok | not ok, different | `hidden_regression` | Heap fails the record, but the engine answers differently again. Counted with regressions |
| not ok | ok | `engine_only_pass` | Should not happen with the same executor; investigate |
| any | `crashed` | `crashed` | Always a failure |
| any | `timeout`, `did_not_run` | as named | Reported, not judged |
| timeout, did not run, crashed | any | `baseline_did_not_run` | Nothing to compare with |
| missing on one side | | `unmatched` | The two runs didn't run the same records |

A listed exception that would otherwise fail is `exception`. Tables on the
wrong AM fail the cell before any comparison.

**Exceptions.** `src/tools/prolly_bench/configs/c_exceptions.toml` lists
records (file and line, per AM or for every AM) that are expected to
differ, each with a reason, as gate 1 documents its exceptions (tests that
print `ctid`). An exception that no longer differs is reported, so the list
doesn't go stale.

**Gate.** Gate 8 in `06-benchmarking.md`: an engine passes the C suite
when it has no `regression`, `hidden_regression`, `crashed` or `unmatched`
records outside the exceptions, and no tables on another AM.
As built, `cgate` is stricter: every class but `agree`,
`postgres_semantics` and listed exceptions fails it, `order_only`,
`not_implemented` and timeouts included.

## Plan

| Step | Work | Done when | Result (2026-10-15) |
|---|---|---|---|
| 1 | `build_slt.sh` and the Go runner, with the changes above; run it by hand against a heap cluster | C1–C3 run on heap; timings for each group measured; the full corpus run once on heap | Done. Timings above; `ddl/` found to be MySQL-only and dropped |
| 2 | The C suite in `prolly_bench`: workloads, suite `C` in the runner, `--am` list, the target (versioned schema for `prolly`), the AM check, `c_smoke.toml` and `c_standard.toml` | `run C --am heap` writes a complete result set; the AM canary works | Done. `prolly` runs in a versioned schema and its tables are checked |
| 3 | Comparison: `table --suite C`, `cdiff`, the classes, the exceptions file | Classes tested on fixtures; heap against itself is all `agree` and `postgres_semantics` | Done (Go tests for the classes, Python tests for the rendering) |
| 4 | Baseline: heap twice on the full corpus | The two runs agree record for record; heap's own failures sorted | Done. The two runs agree on all 5,675,180 records (by sorted answer; see Result digests). Heap passes 99.787%; its 12,113 failures are all `postgres_semantics`, so `c_exceptions.toml` is empty. The largest causes: no unary `+` on text (6,420), refusing to drop a view other views depend on (4,860), division whose integer and numeric results differ from the corpus (about 600), division by zero, and SQLite syntax (triggers, `INSERT OR REPLACE`) |
| 5 | The engines: `prolly`, then `prolly_d1` if still wanted | Result sets and comparisons; regressions filed or fixed | `prolly` passes the gate on the full corpus: 5,663,067 `agree`, 12,113 `postgres_semantics`, nothing else. `prolly_d1` agrees with heap on the smoke config's C2 and C4 |
| 6 | The S and V suites use the target | `run S --am prolly` creates its tables in a versioned schema; the AM check runs there too | Done. S1 and S4 run on `prolly` and heap, V1 on `prolly` (smoke config) |
| 7 | Documentation | Gate 8 in `06-benchmarking.md`, the harness README, `07-roadmap.md` | Done |

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
   throughout. (Since #183 only `heap` and `prolly` exist; `prolly_d1`
   runs from a build of `cff08be5c3`, its last state.)
4. **A suite of `prolly_bench`, not a separate tool.** Recommended: yes; it
   reuses the cluster setup, `--pg-bin`, labels, environment recording and
   the results layout. The C suite has no timed window or collectors, so its
   cells skip both.
5. **CI.** Recommended: not yet. Once the baseline is clean, `c_smoke.toml`
   on `prolly` would fit the lean CI. (Since built: the smoke config and
   `cgate` run on every pull request, the full corpus nightly; spec 16
   proposes the full corpus on every pull request, a heap baseline and a
   results comment.)
