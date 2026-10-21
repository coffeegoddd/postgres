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
src/tools/prolly_bench/prolly_bench run S --am prolly
src/tools/prolly_bench/prolly_bench run V --am prolly

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
Each cell includes its spread and validity details. `compare BASELINE_DIR DIR` gives one result set's ratios
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

# Standard procedure.
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

### Shards

`run C --shard I/N` runs shard I of N, so the full corpus can run as N
parallel jobs:

```sh
src/tools/prolly_bench/prolly_bench cshard --shards 4 --config CONFIG     # the plan
src/tools/prolly_bench/prolly_bench run C --am heap,prolly --shard 2/4 --config CONFIG
src/tools/prolly_bench/prolly_bench cgate BASELINE_DIR ENGINE_DIR       # the shard's workloads
```

Files are assigned largest first, each to the shard with the fewest bytes
so far, so the plan depends only on the corpus's file names and sizes and
every job computes the same one. C1 and C2 run whole on shard 1, so a
shard runs only the workloads it has files of. A shard's `config.json`
records `run.shard`, its workloads, and each workload's `shard_files`;
`cgate` without `--workloads` gates the workloads the shard ran (an
unsharded run still needs C1-C4). A shard's gate covers only its part:
that the shards' files together are the whole corpus exactly once is for
a gate over all of them to check, with `cshard.overlap_and_gaps`. A plan
with an empty shard is refused. prolly_bench takes one run at a time per
host, so shards run in parallel on separate hosts, or one after another
with separate `--out` directories.

### The heap baseline

`cgate` compares an engine with heap from the same build, so a change to
shared code (the planner, the executor, the fork's catalog changes) that
breaks both alike passes it. `configs/c_heap_baseline.tsv.gz` holds heap's
whole outcome on the pinned corpus: one line per record heap doesn't pass,
sorted (`file line status sqlstate sorted-answer-digest`, `-` for an empty
field), after a `# sqllogictest COMMIT` line.

```sh
src/tools/prolly_bench/prolly_bench cbaseline check HEAP_DIR...    # a run, or its shards
src/tools/prolly_bench/prolly_bench cbaseline update HEAP_DIR...   # rewrite from a full run
```

`check` covers the files the run covered, so a shard checks its part. It
fails, in both directions, on a heap failure the file doesn't list or lists
differently (a regression in shared code), and on a listed record that now
passes (update the file, so it stays exact). Records that timed out or
didn't run are never written to the file; a run with any fails as
**incomplete**, not as a regression. `update` refuses anything but a
complete run of the whole corpus.

The corpus pin is `configs/sqllogictest.commit`: CI checks out that commit,
and `cbaseline` refuses a run of another one. **Bumping the pin
regenerates the baseline in the same change:** run heap on the full corpus
twice at the new commit, `cbaseline update` from the first, and `cbaseline
check` the second against it (the two must agree). A change that alters
heap's answers updates the file in the same commit, so the diff shows
exactly which records moved.

### The pull-request comment

```sh
src/tools/prolly_bench/prolly_bench creport SHARD_RESULTS... --shards N \
    --base-baseline BASE_BASELINE --out comment.md [--json summary.json] \
    [--engines prolly] [--head SHA --base SHA --run-url URL --wall-clock '14 min'] \
    [--failed-job NAME]
```

`creport` reads every shard's result sets (heap and each engine) and
writes the sticky comment CI posts, starting with the marker
`<!-- c-suite -->`: a headline (corpus pin, full corpus or smoke, the merge
it ran, records, shards, wall clock, PASS or FAIL), a row per engine (pass
rate, change in failing records against the heap baseline at the base
commit, counts by class, gate), then collapsed sections: new failures (25
per class, each with its query and both sides), fixed records (baseline
entries to remove), stale exceptions, time per cell, and the per-shard
tables. It is cut at 65,000 characters with a notice.

It is also the gate over the shards as a whole, and exits 1 when any shard
1..N is missing or incomplete or lacks an engine of `--engines`, when the
shards' files together aren't the whole corpus exactly once, when an
engine differs from heap, or when heap differs from the baseline. Each
row's gate column fails with the problems that name its engine. It reads
each shard's comparisons where `cgate` left them, and recomputes one
whose inputs changed. `--base-baseline` is the baseline file at the pull
request's base (`git show BASE:src/tools/prolly_bench/configs/c_heap_baseline.tsv.gz`),
so a stacked pull request is compared with the one below it; when the
base has none, or one for another corpus pin, "vs base" shows n/a and a
note, and the report doesn't fail for it. Timeouts and records that didn't
run don't count as failing records against the base.

C configs turn off `fsync`, `synchronous_commit`, `full_page_writes` and
autovacuum: the suite measures correctness and never crashes the server on
purpose.  S and V refuse those settings.

## Counter runs

`configs/s_counters.toml` runs the S workloads as a fixed amount of work
(`pgbench -t`, sysbench `--events`, the TPC-H serial pass) with one client,
fixed seeds, autovacuum and timed checkpoints off, so two runs of one build
count the same: WAL bytes, records and full-page images, `pg_stat_io` for
client backends, relation and chunk-store bytes, `pg_stat_prolly`, write
amplification's totals (`write_amp`), and
instructions where the machine has hardware counters (`instructions`, by
`perf stat`).  Counters are read once the measured clients have exited
(`settled_counters`), so each has flushed its statistics.

```sh
src/tools/prolly_bench/prolly_bench run S --am heap,prolly --config src/tools/prolly_bench/configs/s_counters.toml --out A
src/tools/prolly_bench/prolly_bench run S --am heap,prolly --config src/tools/prolly_bench/configs/s_counters.toml --out B
src/tools/prolly_bench/prolly_bench scounters A/*-S-prolly B/*-S-prolly   # which counters differ
```

S7's counters vary a little: GiST breaks ties between pages at random.
Without hardware counters, `configs/s_cachegrind.toml` counts instructions
with the server under Cachegrind (the `cachegrind` cluster and collector),
on a reduced workload.

### Per-commit growth (V7)

`configs/v_counters.toml` runs V7, commit per transaction: 2,000 one-row
updates of a 100k-row table, each committed as it is made, with the
counters read after each (`pg_stat_prolly`'s fold, node, chunk and
change-log page counters, WAL bytes, and client backends' buffer accesses
from `pg_stat_io`), the load's change-log entries vacuumed away first.  Its
variants commit with `dolt.transaction_commit` in shared and owned writer
modes (`shared`, `owned`), with a `dolt_commit` after each transaction
(`manual`), as manual with another session holding a snapshot open
(`pinned`), and as owned with a `dolt_status` every 100 transactions
(`status`).  `vgrowth` fails a counter whose mean per transaction over the
last tenth is more than 1.5 times its first tenth's (above a floor): work
that grows with the number of commits before it.  The variants that run
autovacuum (every second, so a log is vacuumed within the run) report
WAL bytes, nodes read and chunks appended without gating them: those
count autovacuum's work too.  Known growth, with an issue to fix it, is
listed in `configs/v_growth_accept.toml`, and stands until the fix removes
it: an accepted counter that has stopped growing fails, as does an entry
that names no counter of the run.  Each cell's series is in its raw directory,
`v7-series.json`.

```sh
src/tools/prolly_bench/prolly_bench run V --am prolly --workloads V7 --config src/tools/prolly_bench/configs/v_counters.toml --out G
src/tools/prolly_bench/prolly_bench vgrowth G/*-V-prolly --accept src/tools/prolly_bench/configs/v_growth_accept.toml
```

## Write amplification

`configs/s_write_amp.toml` runs the standard procedure's workloads with the
`write_amp` collector (specs/20-write-amplification.md): for heap and
prolly alike, the bytes the server wrote per byte of row data the
workload wrote.

```sh
src/tools/prolly_bench/prolly_bench run S --am heap,prolly --config src/tools/prolly_bench/configs/s_write_amp.toml
src/tools/prolly_bench/prolly_bench table
```

Logical bytes are (rows inserted + rows updated) x the table's average row
size, from `pg_stat_get_tuples_*()` for the workload's tables. Prolly's
tables run heap's code, so the same counts cover both engines. Deletes
count as rows, not bytes, and rows of aborted transactions count too.

The collector's interval is wider than the window:

- **Before the window** it waits for autovacuum workers to exit, then
  checkpoints, so the warm-up's dirty pages aren't the window's. It
  doesn't wait for clients: S6 and the V suite's background S1 connect
  before the window.
- **After the window** it waits for the clients and autovacuum workers to
  exit. Autovacuum workers report their counts only when they exit. Then,
  on prolly in the S suite, it runs `prolly_checkpoint()`, because a
  plain SQL `COMMIT` writes no chunks and their bytes would otherwise go
  uncounted (`write_amp_materialize`). It then reads the WAL, checkpoints
  again so the window's dirty pages are written, and waits for
  autovacuum once more.

It reports each amount per unit, and as a ratio to the logical bytes:

| Ratio | Bytes written (not synced) |
|---|---|
| `wa_wal` | WAL generated, by LSN difference, split by resource manager with pg_walinspect: rows, indexes, full-page images, prolly's `BlockMap` and `ProllyChunks` records, the rest |
| `wa_pages` | relation pages written and extended by every backend type (`pg_stat_io`, the checkpointer's included), plus prolly's copy-on-write remaps (`wa.pages.bmap_store_bytes`) |
| `wa_tree` | prolly only: the chunk store's journal, table files and metadata (`pg_stat_prolly`), including the copies collection and conjoining made |
| `wa_server` | all three |

Notes on the totals:

- **Chunks are written twice.** Every chunk goes once into WAL
  (`ProllyChunks`) and once into the journal.
- **Extends and bulk writes.** The block map's extends for shared buffers
  (`wa.pages.bmap_extend_bytes`) duplicate `pg_stat_io`'s extends, so
  they aren't added. Its bulk writes (`wa.pages.bmap_bulk_bytes`: index
  builds, copies) aren't added either, because heap's equivalent writes
  count nowhere. Neither engine's `wa_pages` includes bulk writes.
- **What isn't counted:** SLRU writes, and WAL's partial-page rewrites.
- **Small intervals.** Ratios are omitted under 64 KiB of logical bytes,
  and `table` says why for each n/a.
- **Other instances.** A workload's other instances (`ctx.peers`: V4's
  clone, which the clone, fetch and push write to) are measured like the
  origin, and their bytes are added in. `wa.instances` says how many
  instances were counted. Logical bytes are the origin's.

Reports and run constraints:

- **Reports.** `table` shows the ratios and a table of every component per
  engine. Each result set's `README.md` shows that table too.
- **No recovery.** `write_amp` and `recovery` can't share a run, because
  after the second checkpoint recovery has nothing to replay.
  `s_write_amp.toml` also leaves out `vacuum`, which would start just
  after that checkpoint.
- **WAL retention.** A replication slot keeps the window's WAL where
  `wal_level` allows one. That WAL can be tens of GB on a long window.
  The counter runs keep `minimal` and run with no slot, so `wal_keep_size`
  retains 16 GB across S4's explicit checkpoint inside the measured window.
  This retains existing WAL without preallocating 16 GB; each cell starts
  with a fresh cluster. The config refuses S4 with write amplification at
  `minimal` without WAL retention, and requires `checkpoint_timeout` to
  exceed the window. Retention must cover the entire interval; recycled
  WAL still invalidates the result.
- **The gate.** In a counter run, `scounters` gates the logical, WAL and
  chunk-store totals. It doesn't gate the page totals: those sum every
  backend type, and how they repeat depends on timing. `s_counters.toml`
  requires `write_amp`, and the gate fails when any collector that ran in
  the base didn't run in the candidate.

## Comparing builds

`--pg-bin base=DIR,candidate=DIR` runs two builds of the server, a pull
request's base and the pull request, as the variable compared: a result set
per build and engine (`base-B0`, `base-prolly`, `candidate-B0`, ...), the
cells with the same workload, size, clients and repetition run as a group
in random order, with the same seed.  Before every cell, each build's
binaries and provenance are checked against what the run recorded;
`--build-commit base=SHA,candidate=SHA` also checks the commit each build's
provenance names.  `table` then gives each candidate cell's ratio to the
same engine's base cell, with the paired bootstrap interval (5 repetitions
or more), and prolly's ratio to heap in each build.  A comparison of builds
is refused unless everything but the build matches (engine, configuration,
harness, clients, machine).

```sh
src/tools/prolly_bench/prolly_bench run S --am heap,prolly --config src/tools/prolly_bench/configs/s_pr.toml \
    --pg-bin base=build-base/inst/bin,candidate=build-rel/inst/bin --out pr
src/tools/prolly_bench/prolly_bench table pr/*-S-*
```

`configs/s_pr.toml` is a pull request's timing shape: S1, S2 and S10's
read-write test at 8 clients, 5 repetitions, 5 s warm-up and 20 s measured.
Its numbers only compare builds (`comparison_only`), so it may run with
`synchronous_commit = off`; they are never reported as the procedure's.

## The pull-request gate

```sh
# counters: base and candidate counter runs, twice each
src/tools/prolly_bench/prolly_bench sgate counters BASE BASE2 CANDIDATE CANDIDATE2 \
    --accept configs/perf_accept.toml --accept-base base-perf_accept.toml --json counters.json
# timing: a run of both builds (s_pr.toml), then retries of the cells that regressed
src/tools/prolly_bench/prolly_bench sgate timing RUN [RETRY ...] --report-only --json timing.json
src/tools/prolly_bench/prolly_bench sreport --counters counters.json --timing timing.json \
    --head SHA --base SHA --out comment.md
```

A counter regresses when the candidate's is more than 5% above the base's
and, in total over the fixed work, above its floor (`configs/s_gate.toml`);
one that differs between the base's own two runs is reported, not gated, and
one stable in the base but not in the candidate fails.  Fewer is listed as
an improvement.  A timing cell regresses when its whole 95% interval clears
the threshold; one that does in every one of 3 attempts is confirmed, else
transient (hosted runners: reported only).  Missing or failed cells, and
cells whose work differs, always fail.  An entry the pull request adds to
`configs/perf_accept.toml` accepts one regression (entries on the base
accept nothing); the file has a code owner.  `sreport` writes the sticky
comment, marked `<!-- s-suite -->`.

The nightly (`performance.yml`'s nightly job) runs `configs/s_nightly.toml`,
360 cells and some 4 to 5 hours, with tonight's master against the last night
no regression was confirmed on (so a regression stays in the comparison
until it is fixed), retries the cells that regress while the job has time,
and keeps a summary of each night
(`prolly_bench shistory`: medians, ratios with their intervals, commits,
comparison identities) on the orphan `perf-history` branch.

## Calibrating the gate

`prolly_bench scalibrate --counters RUN ... --timing RUN ...` measures the
noise of A/A runs (one build against itself): each counter's stability, the
instruction counts' spread, the timing intervals' widths, and how often the
gate would fail a pull request that changed nothing; it suggests the gate's
settings (`configs/s_gate.toml`).  `.github/workflows/performance-calibration.yml`
runs it on GitHub's hosted runners.

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

Every `run` writes HTML and Markdown automatically after measurement and
evidence finalization. Open `index.html` in a result directory for one
engine/build, or `reports/<invocation-id>/index.html` under the output root
for exactly the engines/builds run together. The output root's `index.html`
and `index.md` list saved runs and invocations, including failed runs.
All HTML is standalone, works from disk without network access, and links
to retained evidence with relative paths. Move the result tree together to
preserve those links. The root catalog uses cached counts and evidence status
from each result directory's `report-status.json`; it does not revalidate
historical raw artifacts. Changed source files invalidate the cache. Run
`report RESULT_DIR` to recheck all evidence and refresh that cached summary.

Reports include a workload overview, coverage and provenance, metric tables
with medians and spread, compatible comparisons and repetition details.
Additional numeric metrics appear even when they have no curated heading;
their units are explicitly unspecified and they receive no performance
ranking. Invalid or missing evidence remains visible. Engine comparisons
use heap within the same build; candidate/base comparisons appear in both
formats. C reports show correctness comparisons when heap is present.
C reports without exactly one heap reference and at least one engine are
still written, but cannot establish a pass; `report` and `table` return nonzero.

```sh
# Regenerate from saved evidence; no database server is needed.
src/tools/prolly_bench/prolly_bench report RESULT_DIR
src/tools/prolly_bench/prolly_bench report RESULT_DIR --format html
src/tools/prolly_bench/prolly_bench report HEAP_DIR PROLLY_DIR --output /tmp/storage
src/tools/prolly_bench/prolly_bench report --manifest ROOT/reports/ID/report.json
src/tools/prolly_bench/prolly_bench report --catalog ROOT
src/tools/prolly_bench/prolly_bench table --suite C --format both
src/tools/prolly_bench/prolly_bench compare HEAP_DIR PROLLY_DIR --format both --output /tmp/comparison
```

`report`, `table`, and the new formatted `compare` mode accept
`--format html|markdown|both`. Single-set reports default to `README.md`
and `index.html`. Explicit output stems produce `.md` and `.html`; multiple
sets require a stem. Existing `compare` without `--format` still prints
Markdown or writes it to the exact `--output` filename.
`run --report-format html|markdown|both|none` chooses automatic formats
(default `both`). `none` retains execution evidence without derived reports.

Invocation manifests record the original plan, exact source directories and
source fingerprints. Regeneration diagnoses replaced evidence. Reports are
derived files outside the execution manifest's hashes; regenerating them
does not invalidate a run. Renderer failures preserve finalized evidence,
still run benchmark validation and comparison gates, and attempt the catalog
update independently; the CLI then returns nonzero. Invocation manifests record
execution and rendering failures separately, including their reasons. Orderly
interruption marks execution `interrupted`; other execution errors mark it
`failed`. Completed result sets retain `complete` even if another set fails.
Reports show the original plan and missing cells. A lock-rejected process does
not write reports or the catalog.

`sreport`, `creport`, and `scalibrate` also save full HTML and Markdown
artifacts alongside their existing comments/reports (the output stem
defaults to `<out-stem>-full`; override it with `--report-output STEM`).
Full reports use the same gate findings and have no comment character limit.
C failure examples retain the analyzer's sampling limit; complete differences
remain in linked evidence. Full-report failures print a diagnostic and preserve
the analyzer's exit status, bounded comment, JSON summary and stdout output.

CI uploads `s-suite-report`, `c-suite-report`, and `s-calibration-report`
with HTML and Markdown. Source evidence stays in the original artifacts,
with the same retention period as its report: 30 days for PR runs, 90 days
for calibration. To use relative evidence links, download artifacts from
the same workflow run into this layout:

| Report artifact → directory | Source artifact → directory |
|---|---|
| `s-suite-report` → `comment/` | Each `s-counters-N` → `counters/s-counters-N/`; `s-timing` → `timing/` |
| `c-suite-report` → `comment/` | Each `c-shard-N` → `shards/c-shard-N/` |
| `s-calibration-report` → `calibration/` | Each `s-calibration-N` → `runs/` (merge their contents) |

Open `comment/report.html` for PR runs or `calibration/report.html` for
calibration. For example, download a C report and its source shards with:

```sh
gh run download RUN_ID --name c-suite-report --dir comment
# Repeat for each c-shard-N artifact from this run:
gh run download RUN_ID --name c-shard-N --dir shards/c-shard-N
```

The `s-nightly` artifact retains its complete tree, with `report.html` at
its root. Existing sticky comment markers, size limits and gate policies
are unchanged.

`specs/results/<date>-<suite>-<label>/`:

| File | Contents |
|---|---|
| `README.md` | Command, config, environment, warnings, collector status, summary tables (median and min-max per cell) |
| `index.html` | Standalone HTML with the same findings and tables as the Markdown report |
| `report-status.json` | Derived counts, evidence findings and source fingerprints for the root catalog |
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
| `Workload` | S1-S10 and S13, V1-V4 and V6, C1-C4 | `pbench/workloads/` |
| `Collector` | wal, diskstats, pg_stat_io, cpu, space, engine_counters, write_amp, instructions, cachegrind, perf, vacuum, recovery | `pbench/collectors/` |
| `Sink` | console, files | `pbench/sinks/` |
| `Cluster` | local, cachegrind | `pbench/clusters/` |

A workload creates its data in `prepare`, may warm up in `warmup`, and does
the measured work inside the window it is given in `measure`, returning an
`Outcome` (metrics, and a transaction count for per-transaction metrics).  A
collector snapshots in `start`/`stop`, computes in `results`, and may act on
the cluster in `post`; `before_window` and `after_window` bracket every
collector's `start` and `stop`, for one whose interval is wider than the
window (`write_amp`).

## Tests

CI (`.github/workflows/correctness.yml`, on every pull request, every push
to `master` and nightly) runs the full C corpus in shards: one build, then
per shard heap and `prolly`, and

```sh
src/tools/prolly_bench/prolly_bench cgate BASELINE_DIR ENGINE_DIR
src/tools/prolly_bench/prolly_bench cbaseline check BASELINE_DIR
```

then the **C suite gate** job runs `creport` over every shard and fails on
any finding or on a missing shard. A pull request changing only `specs/`
passes the gate without building, and its comment says the suite didn't
run. The gate leaves the comment as an artifact, and
`correctness-comment.yml` (a `workflow_run` workflow, with the only write
permission) posts it or updates it in place, unless the run tested an
older commit than the pull request's head. A cancelled run reports
nothing. A failure on `master` or in the nightly run opens or updates one
issue labelled `ci-failure`, and the next pass closes it. Both workflows
use `.github/actions/specs-only` to decide what is specs-only.

All C reporting and gate commands now use the same validation and exit unsuccessfully for missing or
duplicate workloads, empty results, incomplete runs, missing digests,
incorrect AMs, stale exceptions, or differing answers. It requires C1-C4
by default, or a shard's workloads for a shard (`--workloads C1,C2`
selects a smaller explicit set). Order-only
findings also fail until reviewed. The comparator preserves the corpus sort
mode and exact typed answers, independently of rounded corpus formatting.
Documented exceptions and matching PostgreSQL/corpus differences remain
allowed.

PR CI also runs the requirements suites against versioned `prolly`
storage and five differential seeds on heap and `prolly`. The nightly
run increases the seed budget and runs upstream regression, isolation and
recovery against block-mapped storage. Logs, replay programs and summary
records are uploaded on success and failure.

The version-control fuzzer runs seeded histories of branches, commits,
merges (fast-forward, `--no-ff`, conflicts refused, resolved with ours or
theirs in a transaction block, or aborted), cherry-picks, reverts and hard
resets, interleaving one to three sessions, some holding transactions open
across other sessions' steps. The rows are two keyed tables' and a keyless
table's (its copies counted by content); the histories add and drop a
column of the keyed tables, so merges, picks and reverts carry schema
changes (a merge losing a value the other side's dropped column held is
refused). Some writes wait on purpose for the row lock another session's
open transaction holds: the server is checked to show the wait, on that
session, and the write lands as the transaction ends. Each history runs on
a fresh `prolly` database and on a reference model of branches, three-way
row and cell merges, and keyless merges by copies. After every step, it checks the step's outcome, each session's
branch and rows, and whether each branch has uncommitted changes; and that
each branch's head is the commit the model has there, with commits of the
same rows having the same `prolly_root_hash` and commits of different rows
different ones:

```sh
bash src/tools/prolly_bench/build_diff.sh
src/tools/prolly_bench/prolly_bench vcfuzz \
  --pg-bin build-rel/inst/bin --seeds 20 --steps 120 \
  --self-test --reduce 48 --out /tmp/prolly-vcfuzz
```

`--self-test` first proves the checks reject engines wrong in known ways.
A failing history is reduced to a smaller one failing the same way
(`reduced.json`); replay it, or a directory of them kept as regression
cases, with `--replay 'path/*.json'`. The model encodes where prolly's
sessions wait. Histories wait only for row locks, and only where the
outcome is the model's: a keyed row the holder's transaction locked, one
waiter at a time, never a waiter waited on (so nothing deadlocks), and
never a keyless write (it picks its copy before it waits, and may find it
gone after). They stay clear of the other waits: moves of a branch and
schema changes while any transaction is open, and a branch's first
checkout while another session's transaction writes on the branch it is
copied from (Dolt's checkout never waits; prolly's first use of a branch
copies the session's branch's tables under a lock). Doltgres as a third
engine is not run: the `Engine` interface is ready for it.

The differential runner compares seeded transaction histories with heap:

```sh
bash src/tools/prolly_bench/build_diff.sh
src/tools/prolly_bench/prolly_bench differential \
  --pg-bin build-rel/inst/bin --am prolly --seeds 10 --steps 36 \
  --self-test --out /tmp/prolly-differential
```

Each seed gets two fresh databases in an isolated local cluster, with
durability enabled. `--am heap` validates the generator against heap on
both sides.
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
capabilities, named persistent sessions, and ordered steps. Imported cases
can record their source repository, pinned revision, file and original case
name. Unknown fields, operations, capabilities, sessions and forward commit
references are errors.

Capabilities are `sql` and the version-control groups in
`vcscenario/capabilities.go`: `vc.core` (commit, branch, checkout, hard
reset, log, status, hashof, connecting to a branch), `vc.merge`,
`vc.conflicts`, `vc.violations`, `vc.cherrypick`, `vc.revert`,
`vc.diff`, `vc.mergebase`, `vc.gc`, `vc.reflog`, `vc.remotes` (adding,
listing and removing remotes) and `vc.stash`. A scenario requires exactly what
its steps need, no more.

SQL steps are `exec` (no result columns) or `query` (explicit expected rows).
They may call `dolt_*` functions and views; such a step lists the
capabilities its calls need in its own `requires`, which must match what its
SQL names, outside string literals, quoted identifiers and comments (a
`dolt_*` function no capability covers is an error; a table or column of
the scenario's own named `dolt_...` isn't a call). Typed VC steps
exist only where the adapter must capture a commit or open a connection:
`commit` (message, then optional Dolt flags such as `--allow-empty`),
`branch` (name and optional start ref), `checkout` (branch), `reset` (hard-reset ref), and `merge`,
`cherrypick` and `revert` (Dolt's arguments, flags included: `--no-ff`,
`--abort`, `--continue`, ...), and `connect` (Dolt's `USE`: the session's
connection again, to the case's database or, given a branch, to
`database/branch`, a name of at most 63 bytes; a connection refused for
its branch is its SQLSTATE, 42704, and leaves the session as it was, any
other refusal failing the step; the new connection must be on the branch
named, or `main`, and the old backend has exited before the step ends).
Their capability is implied. Arguments are in `args`; SQL bind parameters
are in `params`. Successful commits require a
unique `save` name; a merge, pick or revert may name one, and must then
commit (a revert's is the last of its series): a merge with nothing to merge, or one
that stops with conflicts, saves nothing and fails the step, its call's
whole result in the trace. A later `{"ref":"base"}` resolves to
that engine's captured identifier, including in query parameters and
expected rows. No cross-engine hash equality is assumed. SQL strings remain
explicitly dialect-specific. Merges inside a transaction block, and their
conflicts, are query steps.

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
  --pg-bin build-rel/inst/bin --am prolly --self-test \
  --out /tmp/vc-prolly
```

The bundled fixtures are native harness acceptance cases, not imported Dolt
coverage: one checks core history, one merges, picks, reverts and aborts a
conflicted merge, and one checks SQL values, persistent sessions,
transaction rollback and expected errors. Prolly (a versioned public schema)
declares every capability; heap declares only `sql`. Each
case gets a new database and one persistent connection per named session.
Operations run sequentially; these schedules do not exercise concurrent
blocking queries. VC operations require an idle SQL transaction, since this
adapter snapshots SQL-committed writes. One SQL statement is allowed per
step, using extended-protocol bind parameters. Prepared-plan reuse, schema
history and crash injection are subsequent work. The reviewed
Dolt ports, pinned source inventory, assertion mappings, and execution coverage
reports are described in [corpus/dolt/README.md](corpus/dolt/README.md).

Before running a case, the adapter creates and drops a separate probe
database. It verifies the actual AM (and prolly's table/index/TOAST block-map
forks); for any version-control capability, it runs the built-in history scenario, including
branch isolation and restoration, and for any beyond `vc.core` a second
one (`cases/vc-probe.json`) that finds a merge base, merges, diffs (rows, a summary, statements, a stat, a table's history and rows at a commit), picks, reverts, and reads a
conflict and a constraint violation, reads the reflog, collects garbage, adds and removes a remote, and pushes and pops a stash, each checked by its effect: a claimed
capability that doesn't work fails targeting, not the case. Case fixtures therefore start with fresh
history. The adapter also checks public tables' AMs after steps, deferring
that check until rollback if a SQL error aborted the transaction. Scenario
tables belong in public; arbitrary other schemas are outside this check.

To see coverage gaps, run `--am heap,prolly`.
Any unsupported case makes the command fail, with an explicit `unsupported`
result. An engine's claim of support must also pass the live preflight;
failure there is a failed target, not an automatic exclusion. `--self-test`
checks wrong rows, unexpected SQL errors, a deliberately false VC claim
(on heap), and explicit unsupported reporting.  (No wrong-AM control: a
table of another AM can't be made in public, versioned or not.) These controls are
recorded separately and never added to corpus passes.

Artifacts include the source revision/dirty flag, binary hash, server
settings/log, exact input scenarios and their SHA-256 hashes, declared
engine profiles, step results, and counts of planned/passed/failed/
unsupported/not-run cases. Invalid inputs fail before scenarios execute;
the summary initially records every planned case as not run and updates
after each result. Replay the saved input with `--cases`:

```sh
src/tools/prolly_bench/prolly_bench vc \
  --pg-bin build-rel/inst/bin --am prolly \
  --cases /tmp/vc-prolly/results/scenarios/core-history.json --out /tmp/vc-replay
```

CI runs every fixture on prolly and the SQL fixture on heap. Logs and
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
- Relation/database sizes don't include the chunk store's history. Cluster
  apparent and allocated bytes include it and WAL. Representation size is not
  advertised as cross-engine logical space amplification. Optional collector
  `gc` reports not implemented until prolly's chunk-store GC is built.
- Restart timing describes a process crash with warm OS caches, not power loss.
  Every workload table's typed contents are hashed before/after restart and
  must match. Maintenance runs afterward. Existing durability TAP tests cover
  committed/uncommitted work, prepared transactions and standby recovery.
- Latency summaries retain observation counts and suppress p99 below 1,000
  samples and p99.9 below 10,000. These are sample floors, not tail confidence
  bounds. A pgbench workload can set `rate` and `latency_limit_ms` for scheduled
  arrivals; skipped and failed transactions are separately reported.

V benchmarks require prolly and a successful SQL-visible history-restore
probe. Missing tables fail target checks. Prolly's table/index/TOAST canaries
must have block-map forks. V3 verifies exact change-set and overlap cardinalities;
impossible change sets and oversized V2 changes fail rather than being clamped.

The gate below reads per-operation node work from `*-counters.json`, which
only the tree engine D1 recorded (from its node counters); no engine records
it since D1's removal, so until prolly's folds report theirs the gate
reports `"inconclusive": true` and exits 2 (a failed gate exits 1). For a fixed V2 variant, configure at least four `v_rows`
sizes spanning 8x and five repetitions per size, then inspect/gate
empirical node-work growth:

```sh
src/tools/prolly_bench/prolly_bench complexity RESULT_DIR \
  --workload V2 --operation dolt_commit --counter nodes_read --max-exponent 0.3
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
