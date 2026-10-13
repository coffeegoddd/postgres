# prolly_bench

Benchmark harness for prolly storage: the S suite (storage, any table AM,
compared with stock heap) and the V suite (version control, prolly engines
only).  What it measures and why is in `specs/06-benchmarking.md`.

Python 3.11+ standard library only.  It drives `psql`, `pgbench`, BenchBase
(S9), sysbench (S10), `fio` and `perf`.

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

src/tools/prolly_bench/prolly_bench compare specs/results/<B0 dir> specs/results/<other dir>
```

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
| `Workload` | S1-S10, V1-V4 | `pbench/workloads/` |
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
cd src/tools/prolly_bench && python3 -m unittest discover -s tests
```
