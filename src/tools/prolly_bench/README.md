# prolly_bench

Benchmark harness for prolly storage: the S suite (storage, any table AM,
compared with stock heap), the V suite (version control, prolly engines
only) and the C suite (SQL correctness: sqllogictest, any table AM, compared
with stock heap).

Python 3.11+ standard library only.  It drives `psql`, `pgbench`, BenchBase
(S9), sysbench (S10), `fio`, `perf`, and for the C suite a Go program,
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
better than B0 are green and worse are red (beyond 5%), and hovering a cell
shows its spread.  `compare BASELINE_DIR DIR` gives one result set's ratios
to a baseline in a single wide table.

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
`regression`, `hidden_regression`, `crashed` and `unmatched`.  `cdiff`
lists the differences with the queries.  Records expected to differ go in
`configs/c_exceptions.toml` with a reason.

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
5. post-measurement collectors: space, `VACUUM`, crash recovery;
6. destroy the cluster.

Before the cells it records the environment and runs an fio baseline on the
device under `--work-dir`.

A stub engine's "not implemented" error (SQLSTATE 0A000, read from psql or
the server log) is reported as status `not_implemented`, not as a failure.
Missing tools are `unavailable`, and metrics that don't apply are
`not_applicable`; each says why.

## Results

`specs/results/<date>-<suite>-<label>/`:

| File | Contents |
|---|---|
| `README.md` | Command, config, environment, warnings, collector status, summary tables (median and min-max per cell) |
| `config.json` | The resolved configuration |
| `environment.json` | Machine, kernel, CPU settings, filesystem, device, build, git commits, fio baseline |
| `results.jsonl` | One line per cell: status, message, every metric |
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
| `Workload` | S1-S10, V1-V4, C1-C4 | `pbench/workloads/` |
| `Collector` | wal, diskstats, pg_stat_io, cpu, space, engine_counters, perf, vacuum, recovery | `pbench/collectors/` |
| `Sink` | console, files | `pbench/sinks/` |
| `Cluster` | local | `pbench/clusters/` |

A workload creates its data in `prepare`, may warm up in `warmup`, and does
the measured work inside the window it is given in `measure`, returning an
`Outcome` (metrics, and a transaction count for per-transaction metrics).  A
collector snapshots in `start`/`stop`, computes in `results`, and may act on
the cluster in `post`.

## Tests

```sh
cd src/tools/prolly_bench && python3 -B -m unittest discover -s tests
cd src/tools/prolly_bench/slt && go test .
```
