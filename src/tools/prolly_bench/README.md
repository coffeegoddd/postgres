# prolly_bench

Benchmark harness for prolly storage: the S suite (storage, any table AM,
compared with the fork's heap), the V suite (version control, prolly engines
only) and the C suite (SQL correctness: sqllogictest, any table AM, compared
with the fork's heap).

Python 3.11+ standard library only.  It drives `psql`, `pgbench`, BenchBase
(S9 and S13), sysbench (S10), `fio`, `perf`, and for the C suite a Go program,
slt-runner (`slt/`).

## Quick start

```sh
# once: an optimized build (the harness warns about --enable-cassert builds)
mkdir build-rel && cd build-rel
../configure --prefix=$PWD/inst --with-openssl CFLAGS=-O2
make -j16 && make install && cd ..

# once, and whenever BenchBase changes: build it into build-bench/benchbase/
src/tools/prolly_bench/build_benchbase.sh            # default ~/cursor_src/benchbase

# before a benchmark: CPU governor, boost, perf permissions (needs sudo)
src/tools/prolly_bench/system/system_prepare.sh

src/tools/prolly_bench/prolly_bench run S --am heap          # B0 baseline
src/tools/prolly_bench/prolly_bench run S --am heap --set full_page_writes=off
src/tools/prolly_bench/prolly_bench run S --am prolly_d1
src/tools/prolly_bench/prolly_bench run V --am prolly_d1

# after: put the machine back
src/tools/prolly_bench/system/system_restore.sh

# every engine side by side: a table per metric, a column per engine
src/tools/prolly_bench/prolly_bench table            # S suite; --suite V for V
```

`table` takes the most recent result set per label under `specs/results/`
(or the result sets you name), prints Markdown tables (throughput, latency
percentiles, WAL and device bytes per transaction, CPU, hit ratio, space,
VACUUM, recovery) with each S number's ratio to B0, and writes
`specs/results/summary-S.md` and `summary-S.html`.  In the HTML page, cells
are colored only for benchmark-quality paired repetitions whose 95% bootstrap
interval clears a 5% practical effect threshold. Smoke ratios are descriptive.
Hovering a cell shows its spread and validity details.  `compare BASELINE_DIR DIR` gives one result set's ratios
to a baseline in a single wide table.

## BenchBase TPC workloads

The runnable TPC plugins in the local BenchBase checkout (revision
`a3a748326f4d1e615fd12db4a6ae59a60190a6ee`) are both available:

| Workload | Benchmark | Size setting | Execution |
|---|---|---|---|
| S9 | TPC-C | `tpcc_warehouses` (integer) | Timed mix of NewOrder, Payment, OrderStatus, Delivery and StockLevel; configured client sweep |
| S13 | TPC-H | `tpch_scale` (fractional values supported) | All 22 queries; serial and throughput variants |

TPC-DS is unfinished upstream: it has no registered plugin or runnable
query suite. TPC-E is not provided. CH-benCHmark is a separate hybrid
workload, not one of these TPC plugins. S1 remains pgbench's TPC-B-like
workload. These measurements are not certified TPC results.

```sh
bash src/tools/prolly_bench/build_benchbase.sh

# Both BenchBase TPC workloads on heap and block-mapped prolly storage.
src/tools/prolly_bench/prolly_bench run S --am heap,prolly \
  --workloads S9,S13 --config src/tools/prolly_bench/configs/smoke.toml

# Standard procedure; select an experimental engine with --am prolly_d1, etc.
src/tools/prolly_bench/prolly_bench run S --am heap,prolly --workloads S9,S13
src/tools/prolly_bench/prolly_bench table
```

Set `[workloads.S13] modes = ["serial", "throughput"]` to select variants
(both shipped configs select both). `serial` executes Q1 through Q22 in
order with no time limit; each query runs cold once and then hot once,
with only the hot execution recorded, following BenchBase's untimed
serial procedure. `--measure` does not truncate that pass. With warmup
enabled, it first runs a separate complete, discarded pass. Latency
samples cover the hot executions; the collector window also includes
the cold executions and client startup.
`throughput` randomly selects from all 22 queries for the configured
warmup and measurement durations. A short throughput run need not visit
every query; the serial variant verifies complete coverage.

Both TPC-H variants use one client, regardless of `--clients`: upstream
Q15 creates and drops the shared `revenue0` view, which collides across
concurrent workers. TPC-H reads user data, but Q15 writes catalogs, so
WAL and device-write metrics remain applicable. Data loading and ANALYZE
happen before measurement. Smoke scale is 0.01; standard scales are 1
and 100 for the in-memory and larger-than-RAM configurations respectively.

Results retain BenchBase summaries and transaction outcome counts in
`raw/.../benchbase-measure/`, plus TPC-H `queries.json` with each query's
count, mean latency and total time. BenchBase `tps` and throughput-mode `qps`
count successful operations over the full client process, matching the resource
counter scope. Upstream active-phase attempts and reported rates are retained as
`measured_requests`, `client_reported_tps` and `client_reported_goodput_tps`;
upstream goodput combines different scopes and can exceed its throughput.
Latency samples describe active-phase attempts, including failed attempts.
Serial mode reports `hot_query_seconds` for Q1–Q22 and `client_process_qps`
for both cold and hot passes including process overhead; resource counters
use all 44 executions as their denominator. Logged loader errors,
unexpected SQL errors, empty runs, or incomplete serial query coverage
fail the cell rather than producing successful timing results. Engine
features that are not implemented still receive the harness's explicit
`not_implemented` status. PostgreSQL deadlocks (`40P01`) in TPC-C are
reported as `failed_transactions`: this BenchBase revision classifies
them as unexpected errors rather than retrying them. Other unexpected
SQL errors still fail the cell.

## The C suite

```sh
# once, and whenever slt/ changes (needs Go): build-bench/slt/slt-runner
src/tools/prolly_bench/build_slt.sh

# heap (B0) and an engine, then the comparison; about 5 minutes
src/tools/prolly_bench/prolly_bench run C --am heap,prolly

# about 50 seconds per AM: C1 and C2 whole, a file per directory of C3 and C4
src/tools/prolly_bench/prolly_bench run C --am heap,prolly --config src/tools/prolly_bench/configs/c_smoke.toml

src/tools/prolly_bench/prolly_bench table --suite C          # latest per label
src/tools/prolly_bench/prolly_bench cdiff specs/results/<B0 dir> specs/results/<engine dir>
```

The corpus is Dolthub's sqllogictest (`~/cursor_src/sqllogictest`, set in
the config's `[tools]`): C1 `select1`-`select5`, C2 `evidence/`, C3
`index/`, C4 `random/`; 5,675,180 records run on Postgres.  Each cell runs
its part through slt-runner, which runs every test file in a fresh database
over one session, a file per worker process (one per CPU), and records each
record's status with a digest of the answer it got.

An engine is judged by agreement with heap, not only by the corpus's
expected answers: `table --suite C` gives the pass rate per engine, then
each engine's records in classes against B0 (`agree`, `order_only`,
`regression`, `postgres_semantics`, ...), and a gate that fails on
every class other than `agree`, `postgres_semantics`, or a precisely matched
exception. `cdiff` lists differences with the queries. Exceptions in
`configs/c_exceptions.toml` must name the engine, exact actual-answer signatures,
corpus file hash, owner, issue and expiry. Changed or stale exceptions fail.

C configs turn off `fsync`, `synchronous_commit`, `full_page_writes` and
autovacuum: the suite measures correctness and never crashes the server on
purpose.  S and V refuse those settings.

## Targets

`prolly` tables exist only in versioned schemas, and with `prolly` as the
default table AM a table created anywhere else is a heap table.  So for
`prolly`, every suite creates its tables in a versioned schema
(`pbench/core/targets.py`): S and V make it first on the benchmark
database's `search_path`, and the C suite runs the corpus in it.  Every
cell checks that its tables are on the AM under test, and fails otherwise.

## A quick check

A quick check that everything works (tiny data, 10-second windows):

```sh
src/tools/prolly_bench/prolly_bench run S --am heap --config src/tools/prolly_bench/configs/smoke.toml
```

`prolly_bench run --help` lists the overrides (`--workloads S1,S3`,
`--clients`, `--reps`, `--sizes`, `--measure`, `--set name=value`,
`--work-dir`, ...).

## What a run does

For each cell (workload x size x variant x client count x repetition):

1. initdb a fresh cluster in `--work-dir` (default `build-bench/work`, which
   the harness makes copy-on-write-free on btrfs), with the config's
   `[postgres]` settings;
2. create the workload's data, with the table AM under test as
   `default_table_access_method`, then `CHECKPOINT`;
3. warm up (discarded);
4. measure inside a window; collectors snapshot as it opens and closes;
5. check workload invariants and optional TPC-H answers, verify crash recovery
   before maintenance, then measure space, `VACUUM` and optional GC;
6. destroy the cluster.

Before the cells it records the environment and runs an fio baseline on the
device under `--work-dir`.

A stub engine's "not implemented" error (SQLSTATE 0A000, read from psql or
the server log) is reported as status `not_implemented` and fails the execution
gate for the requested plan. It does not count as tested.
Missing tools are `unavailable`, and metrics that don't apply are
`not_applicable`; each says why.

## Results

`specs/results/<date>-<suite>-<label>/`:

| File | Contents |
|---|---|
| `README.md` | Command, config, environment, warnings, collector status, summary tables (median and min-max per cell) |
| `config.json` | The resolved configuration |
| `environment.json` | Machine, kernel, CPU settings, filesystem, device, build, git commits, fio baseline |
| `run.json` | Versioned plan, completion state, comparison identity and artifact hashes |
| `results.jsonl` | One line per cell: execution status, validation errors, metrics, completed unit/denominator, measurement scope |
| `raw/` | Per cell: client program output, latency histograms (`latency-*.json`), compressed server log, BenchBase summaries, perf report |

Large intermediate output (per-transaction latency logs, BenchBase samples,
`perf.data`) goes to a per-cell directory in `/dev/shm` and is deleted after
the cell, so it neither touches the device being measured nor bloats the
results.

The label defaults to `B0` for heap (`B0-fpw-off` with
`full_page_writes=off`) and the table AM name otherwise.

## Extending

Everything is an interface (`pbench/core/interfaces.py`) with implementations
found through registries (`pbench/core/registry.py`).  Adding one is adding a
file; the driver doesn't change.

| Interface | Implementations | Directory |
|---|---|---|
| `Workload` | S1-S10 and S13, V1-V4, C1-C4 | `pbench/workloads/` |
| `Collector` | wal, diskstats, pg_stat_io, cpu, space, engine_counters, perf, vacuum, recovery | `pbench/collectors/` |
| `Sink` | console, files | `pbench/sinks/` |
| `Cluster` | local | `pbench/clusters/` |

A workload creates its data in `prepare`, may warm up in `warmup`, and does
the measured work inside the window it is given in `measure`, returning an
`Outcome` (metrics, and a transaction count for per-transaction metrics).  A
collector snapshots in `start`/`stop`, computes in `results`, and may act on
the cluster in `post`.

## Tests

CI runs the C smoke corpus on heap and `prolly`, then calls:

```sh
src/tools/prolly_bench/prolly_bench cgate BASELINE_DIR ENGINE_DIR
```

All C reporting and gate commands now use the same validation and exit unsuccessfully for missing or
duplicate workloads, empty results, incomplete runs, missing digests,
incorrect AMs, stale exceptions, or differing answers. It requires C1-C4
by default (`--workloads C1,C2` selects a smaller explicit set). Order-only
findings also fail until reviewed. The comparator preserves the corpus sort
mode and exact typed answers, independently of rounded corpus formatting.
Documented exceptions and matching PostgreSQL/corpus differences remain
allowed.

PR CI also runs the requirements suites against versioned `prolly`
storage and five differential seeds on heap, `prolly`, and D1. The nightly
run increases the seed budget and runs upstream regression, isolation and
recovery against block-mapped storage. Logs, replay programs and summary
records are uploaded on success and failure.

The differential runner compares seeded transaction histories with heap:

```sh
bash src/tools/prolly_bench/build_diff.sh
src/tools/prolly_bench/prolly_bench differential \
  --pg-bin build-rel/inst/bin --am prolly --seeds 10 --steps 36 \
  --self-test --out /tmp/prolly-differential
```

Each seed gets two fresh databases in an isolated local cluster, with
durability enabled. `--am heap` validates the generator against heap on
both sides; `prolly_d1` and the other experimental AMs use the same runner.
The target canary checks the actual AM, and for `prolly` checks the table,
index and TOAST block-map forks. Unsupported engines fail; they do not
count as tested. `--self-test` proves a deliberate answer difference is
rejected (using `prolly` as the candidate when `--am heap` is selected).

Every 18 groups covers upserts, updates, deletes, nested savepoints,
transaction aborts, DDL, expression indexes, rewrites, transactional
truncate, vacuum/reindex, cursors, triggers, queries, NULL/empty values,
prepared-plan reuse, duplicate-heavy keyless tables, typed edge values,
and independent tables. Dataset cardinality also varies with the seed.
Rows include exact numerics beyond 2^53 and large incompressible text.
Results compare command tags, column types, SQLSTATEs, transaction state,
row counts and ordered row digests. Only explicitly declared errors are
allowed; matching unexpected errors on both sides fail. After each group,
the runner compares all fixture rows and checks a sequential scan against
a verified index scan within each target. Queries that return rows have
defined ordering.

The output directory must be new. It contains the revision, dirty flag,
server version/settings, runner binary hash, server log, seed programs,
result records, and a summary separating passed, failed and unexecuted
seeds. On a failure, replay the saved program, not just the seed, so future
generator changes cannot change the reproducer:

```sh
src/tools/prolly_bench/prolly_bench differential \
  --pg-bin build-rel/inst/bin --am prolly \
  --replay /tmp/prolly-differential/cases/seed-1/program.json \
  --reduce 48 --out /tmp/prolly-replay
```

Reduction removes whole transaction groups and retains a candidate only
when the same failure signature recurs. Setup is preserved. The original
program remains available; `reduced.json` is a bounded reduction, not a
guaranteed minimum. Timeouts, crashes and setup errors fail the run and
retain the program but are not reduced. This runner is single-session SQL
testing: concurrency, history operations, and injected crash points belong
to separate suites.

```sh
cd src/tools/prolly_bench && python3 -B -m unittest discover -s tests
cd src/tools/prolly_bench/slt && go test ./...
```

### Version-control scenario contract

`slt/internal/vcscenario` defines version 1 of the shared scenario format
and an engine-adapter interface. A scenario has an ID, description, required
capabilities (`sql`, optionally `vc.core`), named persistent sessions, and
ordered steps. Imported cases can record their source repository, pinned
revision, file and original case name. Unknown fields, operations,
capabilities, sessions and forward commit references are errors.

SQL steps are `exec` (no result columns) or `query` (explicit expected rows).
VC steps are `commit` (message), `branch` (name and optional start ref),
`checkout` (branch), and `reset` (hard-reset ref). Arguments are in `args`;
SQL bind parameters are in `params`. Successful commits require a unique
`save` name. A later `{"ref":"base"}` resolves to that engine's captured
identifier, including in query parameters and expected rows. No cross-engine
hash equality is assumed. SQL strings remain explicitly dialect-specific.

Cells and parameters are strings, JSON null, or commit references. Numbers
are strings so exact numerics do not pass through JSON floating point.
Query expectations specify `order: "ordered"` or `"bag"`; bag comparison
preserves duplicate multiplicity. Optional column names, command tags,
SQLSTATEs and transaction statuses (`I`, `T`, `E`) strengthen assertions.
Unexpected SQL errors always fail, including errors shared by engines.

An adapter declares capabilities, verifies its target, opens fresh scenario
state and named sessions, executes operations, checks storage targeting,
and cleans up. Missing declared capabilities report `unsupported`, never
`passed`. Failed targeting, SQL assertions, transport and cleanup report
`failed`; runtime errors never become automatic capability exclusions.
The result contains the failing phase and an ordered step trace, including
actual rows and SQLSTATEs. The runner stops that case at its first failure.
This format has no implicit skips or result-check bypasses.

Run the PostgreSQL adapters with:

```sh
bash src/tools/prolly_bench/build_vc.sh
src/tools/prolly_bench/prolly_bench vc \
  --pg-bin build-rel/inst/bin --am prolly_d1 --self-test \
  --out /tmp/vc-d1
```

The bundled fixtures are native harness acceptance cases, not imported Dolt
coverage: one checks core history, the other checks SQL values, persistent
sessions, transaction rollback and expected errors. D1 declares `sql` and
`vc.core`; heap and prolly declare only `sql`; D2-D4 declare neither. Each
case gets a new database and one persistent connection per named session.
Operations run sequentially; these schedules do not exercise concurrent
blocking queries. VC operations require an idle SQL transaction, since this
adapter snapshots SQL-committed writes. One SQL statement is allowed per
step, using extended-protocol bind parameters. Prepared-plan reuse, merge,
schema history and crash injection are subsequent work. The first reviewed
Dolt ports, pinned source inventory, assertion mappings, and execution coverage
reports are described in [corpus/dolt/README.md](corpus/dolt/README.md).

Before running a case, the adapter creates and drops a separate probe
database. It verifies the actual AM (and prolly's table/index/TOAST block-map
forks); for `vc.core`, it runs the built-in history scenario, including
branch isolation and restoration. Case fixtures therefore start with fresh
history. The adapter also checks public tables' AMs after steps, deferring
that check until rollback if a SQL error aborted the transaction. Scenario
tables belong in public; arbitrary other schemas are outside this check.

To see coverage gaps, run `--am heap,prolly,prolly_d1,prolly_d2,prolly_d3a,prolly_d3b,prolly_d4`.
Any unsupported case makes the command fail, with an explicit `unsupported`
result. An engine's claim of support must also pass the live preflight;
failure there is a failed target, not an automatic exclusion. `--self-test`
checks wrong rows, unexpected SQL errors, wrong AMs, a deliberately false
prolly VC claim, and explicit unsupported reporting. These controls are
recorded separately and never added to corpus passes.

Artifacts include the source revision/dirty flag, binary hash, server
settings/log, exact input scenarios and their SHA-256 hashes, declared
engine profiles, step results, and counts of planned/passed/failed/
unsupported/not-run cases. Invalid inputs fail before scenarios execute;
the summary initially records every planned case as not run and updates
after each result. Replay the saved input with `--cases`:

```sh
src/tools/prolly_bench/prolly_bench vc \
  --pg-bin build-rel/inst/bin --am prolly_d1 \
  --cases /tmp/vc-d1/results/scenarios/core-history.json --out /tmp/vc-replay
```

CI runs both fixtures on D1 and the SQL fixture on heap/prolly. Logs and
coverage artifacts are uploaded with the existing engine results.


## Evidence and quality contract

Use `--quality smoke` (the default) for functional integration work. Use
`--quality benchmark` only on a prepared, idle host and a real data device.
Benchmark mode requires at least five repetitions, warmup, runtime host
monitoring, and matching build provenance. It rejects environment warnings,
including tmpfs, uncontrolled CPU settings, assertion builds and dirty source.
A profile is a set of necessary checks, not a guarantee that external noise
or every engine defect has been eliminated.

Build an attested release install from an unchanged checkout:

```sh
bash src/tools/prolly_bench/build_postgres.sh \
  --build-dir "$PWD/build-bench/quality-server" -- --with-openssl
```

The build directory must be new. `inst/pbench-build.json` binds installed
binaries to hashes of the actual source files and configure command. Runs
also retain server/client/JAR hashes and effective `pg_settings`. B0 means
the heap implementation in the selected build; it is not an independently
built upstream PostgreSQL baseline.

Before executing, the harness writes every planned cell. Completion is an
atomic manifest containing hashes of config, environment, results and raw
evidence. Paths to raw artifacts are relative so an artifact can be moved.
Interrupted runs remain incomplete. Failures, missing repetitions, invalid
numbers, mandatory collector failures and unavailable requested workloads
cannot disappear from averages. Legacy artifacts remain readable but cannot
establish valid comparisons; rerun them with this schema.

```sh
src/tools/prolly_bench/prolly_bench gate RESULT_DIR
src/tools/prolly_bench/prolly_bench gate --baseline HEAP_DIR ENGINE_DIR
```

These validate execution evidence; C correctness additionally uses `cgate`.
Comparisons require identical workload parameters, settings, harness/client
versions, build mode and machine identity. Server binaries may differ and
are recorded explicitly. Several engines run as adjacent matched cells,
with seeded random pair and engine order recorded in `raw/experiment-order.json`.
Repetitions have paired workload seeds. pgbench, sysbench and BenchBase all
receive explicit seeds. Background traffic ramps up outside the timed window.

TPC-H additionally runs Q1–Q22 in a separate untimed serial pass using the
instrumented BenchBase build. `tpch-answers.jsonl` retains query-parameter,
column-type, row-count, ordered-answer and row-multiset digests. Values are
length-framed, distinguish NULL from empty bytes and retain numeric precision.
Both passes of every query are required. Run/table/compare with a heap baseline
reject differing answers or parameters. Rebuild BenchBase after updating the
harness; the checked instrumentation hook fails if upstream source changes.
TPC-C checks loaded scale and warehouse/district balance invariants. Neither
these checks nor successful execution constitutes TPC certification.

Metric scopes are explicit:

- `window_seconds` spans client execution including startup/shutdown, with
  per-collector snapshot timestamps retained. pgbench log parsing occurs
  outside that window. `pg_stat_io` snapshots can lag backend flushes; no
  collector sleeps and extends other counters' stop boundaries.
- Resource `*_per_operation` fields use the cell's recorded `unit` and
  `denominator`. COPY-only throughput is separate from total bulk-load
  throughput including index construction and checkpoint.
- CPU and perf summaries are labeled as host activity. Server CPU is withheld
  until a reliable server-specific source is available. Device counters cover
  the entire data device and may include unrelated or delayed writes.
- Relation/database sizes are not complete D1 storage sizes. Cluster apparent
  and allocated bytes include global engine stores and WAL. Representation
  size is not advertised as cross-engine logical space amplification.
  Optional collector `gc` records D1 node retention/reclamation and cluster
  allocated bytes before/after GC, after the other measurements.
- Restart timing describes a process crash with warm OS caches, not power loss.
  Every workload table's typed contents are hashed before/after restart and
  must match. Maintenance runs afterward. Existing durability TAP tests cover
  committed/uncommitted work, prepared transactions and standby recovery.
- Latency summaries retain observation counts and suppress p99 below 1,000
  samples and p99.9 below 10,000. These are sample floors, not tail confidence
  bounds. A pgbench workload can set `rate` and `latency_limit_ms` for scheduled
  arrivals; skipped and failed transactions are separately reported.

V benchmarks currently require D1 and a successful SQL-visible history-restore
probe. Missing tables fail target checks. Prolly's table/index/TOAST canaries
must have block-map forks. D1 operation-level node reads/writes are sampled in
the same timed SQL session and stored in `*-counters.json`, not sampled from a
separate backend. V3 verifies exact change-set and overlap cardinalities;
impossible change sets and oversized V2 changes fail rather than being clamped.

For a fixed V2 variant, configure at least four `v_rows` sizes spanning 8x and
five repetitions per size, then inspect/gate empirical node-work growth:

```sh
src/tools/prolly_bench/prolly_bench complexity RESULT_DIR \
  --workload V2 --operation prolly_commit --counter nodes_read --max-exponent 0.3
```

The report includes a bootstrap interval for the measured exponent. The bound
is an explicit experiment requirement, not an inferred asymptotic guarantee.
History depth, branch count, table/index count, concurrency and crash-fault
coverage remain distinct dimensions; an N sweep cannot certify those claims.

CI runs release-build client integrations (`configs/integration.toml`), runtime
invariants and recovery checks; nightly jobs run the full pinned SQLLogicTest
corpus, larger differential coverage and ASan/UBSan storage/durability tests.
A manual controlled-performance job is available only when
`PROLLY_BENCHMARK_RUNNER_ENABLED=true`, a `prolly-benchmark` self-hosted runner
is provisioned, and `PROLLY_BENCHMARK_DATA_ROOT` names its data device directory.
This job does not change host CPU or kernel settings automatically.
