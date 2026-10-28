# Cross-engine version-control benchmarks

Builds isolated Dolt, Doltgres, and this fork's **prolly** databases, then
measures branch creation, checkout, soft reset, and row diffs using persistent
SQL connections. Linux, Python 3.11+, Go, and PostgreSQL build dependencies are
required. Python uses only the standard library; Go dependencies are pinned in
`driver/go.mod` and `driver/go.sum`.

The default data root is `~/dbs/benchmarks`. It never connects to your existing
database services, modifies global Dolt configuration, or drops databases.
Servers bind to loopback on dedicated ports. They use local benchmark credentials
(Dolt root with no password, Doltgres postgres/password, PostgreSQL trust auth).

## Choose the database directory

Pass `--root` **before the subcommand** on each invocation:

```sh
python3 src/tools/vc_bench/vc_bench.py --root /mnt/fast-disk/vc-bench init
python3 src/tools/vc_bench/vc_bench.py --root /mnt/fast-disk/vc-bench build
python3 src/tools/vc_bench/vc_bench.py --root /mnt/fast-disk/vc-bench prepare
python3 src/tools/vc_bench/vc_bench.py --root /mnt/fast-disk/vc-bench run
```

The database directories are `ROOT/dolt/data`, `ROOT/doltgres/data`, and
`ROOT/prolly/data`. Configuration, builds, logs, and results also live under
`ROOT`. Source locations and existing binary paths can be set separately in
`ROOT/config.json`. A different root selects an independent experiment; it does
not move existing databases. When manually moving an existing root, update its
absolute binary paths in the configuration before using it again.

## Check prerequisites before starting

`build`, `prepare`, `run`, and `start` automatically check **all selected engines**
before starting the first one. Run the same checks separately with:

```sh
python3 src/tools/vc_bench/vc_bench.py check --for build
python3 src/tools/vc_bench/vc_bench.py check --for prepare
python3 src/tools/vc_bench/vc_bench.py check --for run
# --root and --engines work here too; default check stage is prepare.
```

Checks report all detected failures together and exit nonzero. They cover:

- Python 3.11+ and Linux at CLI startup.
- **Build:** Go/git and source paths; Go module/toolchain resolution; working
  C/C++ compilers and ICU headers/libraries for the Go engines; make, Bison,
  Flex, Perl, pkg-config, and the actual PostgreSQL configure checks for compiler
  versions and development libraries such as OpenSSL, ICU, readline, and zlib.
- **Prepare/start/run:** executable engine binaries, dynamic library loading,
  PostgreSQL companion programs and installed bootstrap files, binary provenance
  where recorded, writable directories, and available ports.
- **Prepare/run:** the SQL driver can execute; configured memory collection can
  read Linux `/proc` counters. **Run** additionally checks that every selected
  fixture is prepared and matches the requested configuration.

The checks do not start servers or load data. Build checks compile tiny temporary
compiler/linker probes and run PostgreSQL configure in a temporary directory;
Go may download required modules/toolchains. No system packages are installed.
Free disk space is printed for planning, not treated as a guarantee of capacity:
physical data, history, WAL, and temporary-space requirements vary by engine.
Successful checks cannot guarantee a later build or every SQL capability will
succeed; live semantic checks still run during preparation and measurement.
`check --for build` tests dependencies even for an existing build; an actual
PostgreSQL rebuild still requires a fresh build directory.

## Create the large fixtures

From the PostgreSQL repository root:

```sh
B=src/tools/vc_bench/vc_bench.py
python3 "$B" init
# Optionally edit ~/dbs/benchmarks/config.json before building/loading.
python3 "$B" build --jobs 16
python3 "$B" prepare
python3 "$B" run
```

`build` compiles the local source checkouts, defaulting to `~/cursor_src/dolt`,
`~/cursor_src/doltgres` (or `doltgresql` if present), and this PostgreSQL checkout.
It records commands, source revisions/dirty status, and binary hashes. The
PostgreSQL builder also hashes its actual source files and installed libraries.
The release build enables optimization and keeps durability enabled. Builds and
their logs live under the data root, leaving the source checkout's existing
builds alone. Finish editing source before building; source changes during a
build invalidate its provenance. PostgreSQL needs a fresh `build/postgres`
directory for a rebuild; use another benchmark root to retain both builds.

You can instead set each engine's `binary` in `config.json` to an existing
executable. Still build the SQL driver with:

```sh
mkdir -p ~/dbs/benchmarks/bin
(cd src/tools/vc_bench/driver && go build -trimpath -o ~/dbs/benchmarks/bin/sql-driver .)
```

External binaries are recorded by hash and labeled as having unverified source
provenance. To reuse build artifacts in another root, copy the matching
`build/ENGINE.json` provenance files too. Do not replace a binary under a running
server. No dependencies or kernel settings are installed/changed automatically.

Default fixture sizes are **0.01, 0.1, 1, and 10 decimal GB per engine**. A row has
an int64 primary key, an int64 revision, and 1,024 bytes of deterministic,
row-specific base64 payload. Logical size is `rows * (payload_bytes + 16)`,
excluding engine encoding, indexes, MVCC, WAL, and history. The largest table has
9,615,384 rows (9,999,999,360 logical bytes). Physical storage will differ among
engines; sizing all engines to identical physical bytes would give unequal row
counts. The random-looking payload has about six bits of entropy per byte,
avoiding misleading repeated-string compression/deduplication.

The base data totals about 33.3 logical GB across all three engines. Allow
substantial additional space for indexes, history, temporary storage, WAL, and
builds. `prepare` reports progress every 15 seconds during loading. It uses
bounded multi-row INSERT batches (default 1,000 rows, one SQL transaction each),
without a 10 GB intermediate file. Batch loading is deliberately outside the
measured operations. Identical SQL-visible contents and primary keys are loaded
in every engine; no secondary indexes are created.

Each size is a separate database named `bench_nROWS`; its table is `bench_rows`.
`prepare` retains the data and commit history for manual experiments. Completed
fixtures are skipped on rerun. Interrupted base loads resume after checking that
the stored IDs form a contiguous prefix. History creation checkpoints every
commit; if a commit finished before its checkpoint was saved, the tool fails
with a HEAD mismatch for inspection instead of silently constructing a different
history. It never silently adopts an unmanaged database.

## Smoke check

```sh
python3 "$B" --root ~/dbs/benchmarks/smoke init --smoke
python3 "$B" --root ~/dbs/benchmarks/smoke build --jobs 16
python3 "$B" --root ~/dbs/benchmarks/smoke prepare
python3 "$B" --root ~/dbs/benchmarks/smoke run
```

Smoke uses four tiny sizes, four historical commits, and two repetitions. It
validates integration and operation semantics; its scaling estimates are
intentionally inconclusive. `--engines dolt,doltgres,prolly` selects a subset
for build, prepare, run, start, stop, or status. `prepare` and `run` automatically
start and stop one engine at a time. Servers already started with `start` are
left running. Stop other benchmark servers before a measurement run to avoid
cross-engine interference, and keep this host otherwise idle.

## Experiment dimensions

Edit `config.json` before `prepare`. `rows` overrides `sizes_gb` when nonempty.
The payload width remains fixed across a table-size sweep. Changing width requires
a new root; it cannot be applied to existing data. You can add new sizes and new
change counts to an existing root and run `prepare` again.

| Dimension | Config | Default |
|---|---|---|
| Table size N | `sizes_gb` or explicit `rows` | 0.01, 0.1, 1, 10 GB |
| Changed rows K | `changes` | 1,000 |
| Pending dirty rows Q | `dirty_sizes` | xsmall 0.001%, small 0.01%, medium 1%, large 10% of N |
| Distance D from current HEAD | `history_depths` | 32, 100, 500 commits (smoke: 1, 4) |
| Repetitions | `repetitions`, `warmups` | 1 measured per pass; no discarded warmups (set `warmups` to run some first) |
| Memory sampling | `sample_interval_ms` | 10 ms |

Every commit updates the **same K evenly spaced primary keys**, setting their
revision to the commit number. Thus a historical diff returns K rows regardless
of distance. Working changes set these keys to one higher revision; the payload
is unchanged. This is an update-only, distributed-key workload. It does not claim
to characterize inserts/deletes, schema changes, keyless tables, secondary
indexes, contiguous edits, or histories with growing disjoint change sets.

For a K sweep, use e.g. `"changes": [1, 10, 100, 1000]`; each K gets a separate
history branch from the same loaded base. K must fit the smallest table and is
never clamped. Depth experiments keep N and K fixed. The harness resolves and
saves actual commit hashes before timing version-control operations, isolating
their cost from ref parsing/traversal. `resolve_ancestor` separately measures
`HASHOF('HEAD~D')` (the fork uses `dolt_hashof`). Total fixture history length is
the maximum configured distance and is recorded in every sample.

| Operation | Timed statement / behavior |
|---|---|
| `round_trip` | `SELECT 1`, connection/protocol overhead reference |
| `branch`, D=0 or D>0 | Create a new branch at HEAD or an older saved hash |
| `checkout`, D=0 | Switch to another branch with the same root |
| `checkout`, D>0 | Switch to a branch with an older root |
| `reset_soft` | Reset a scratch branch's HEAD to the current/older commit, retaining its working rows |
| `reset_hard` | Reset a scratch branch's HEAD and working set to the current/older commit, discarding its working rows (with a dirty working set at D=0: discard the pending changes; see below for D>0) |
| `diff_equal` | Full count of row diff between identical commit hashes |
| `diff_working` | Full count of HEAD versus dirty WORKING |
| `diff_history` | Full count of old hash versus current hash |
| `diff_working_history` | Full count of old hash versus dirty WORKING |
| `resolve_ancestor` | Resolve `HEAD~D` without a diff |
| `branch_delete`, D=0 or D>0 | Delete (`-D`) a branch made at HEAD or an older saved hash and never checked out |
| `branch_delete_used`, D=0 or D>0 | The same, for a branch checked out once (and left) before the timed delete, so the engine has made what a session on it needs |
| `drop_table` | `DROP TABLE bench_rows` on the scratch branch, clean or with a dirty working set; a hard reset to HEAD brings it back, untimed |
| `at_history`, D=0 or D>0 | Read three changed rows as the saved hash has them (prolly: `dolt_at(hash, NULL::bench_rows)`; Dolt and Doltgres: `bench_rows AS OF hash`), by key |
| `merge_ff`, D>0 | The scratch branch, hard reset (untimed) to the saved hash, merges the fixture branch: a fast-forward to HEAD |
| `merge`, D>0 | A branch made at the saved hash, with one commit (untimed) changing rows no fixture commit changes, merges the fixture branch (HEAD): a three-way merge with an in-window base, its merge commit made; the branch is deleted after, untimed, so neither commit keeps a root |

### The history window

Operations are measured inside the history window, the optimal path for
version control: every commit a run targets keeps a root of its pages, so
a move to it gives the pages back rather than writing rows. `window.py`
holds the settings and the checks; each engine answers them through
`engines.py` (Dolt and Doltgres keep all history locally and have no
window: their targets are all in it).

- **The window holds all the history a run builds, at every size.**
  `prepare` sets it on each prolly database before it builds any history
  (a root let go of, its commit outside the window, isn't kept again), and
  `run` sets it again before connecting: `prolly.history_window` (100
  years by default), `prolly.history_min_commits` (the deepest history
  measured plus the baseline: every commit a fixture builds), no root size
  cap and no chunk-store cap (`prolly.history_max_size`), nothing severed
  (`prolly.history_window_action` none), and a root at every commit (`prolly.root_interval` 0,
  `prolly.root_interval_commits` 1). Override them in `config.json`'s
  `history_window` (`time`, `min_commits`, `root_interval_ms`,
  `root_interval_commits`); `min_commits` below what a fixture builds is
  refused. A fixture whose history was built under other window settings
  is refused too: prepare it into a new root.
- **Before each operation**, its target (the saved hash at distance D; at
  D=0 the head) is checked with `prolly_history_in_window()` and
  `prolly_history_roots()`: in the window, and keeping a root, or (with a
  root interval over one commit) one on a commit before it within the
  interval. A target with neither fails the run before it is measured. A
  target outside the window fails it too, unless `history_window.allow_past`
  is true: then it is measured in a group of its own, labelled
  `[past the history window]` (`window` "past" in the samples and CSVs),
  never in the window's medians.
- **After each operation**, in every pass, pg_stat_prolly's path counters
  are read (outside timing) and must show the root path: no
  `moves_root_missing` (a move to a commit without a root), no
  `restores_lock_busy` (a restore given up for a lock), no
  `severed_refusals` (a read refused, or rows left out, past the history
  horizon), no `move_tables_written` but for a move from the nearest root,
  and a hard reset to D>0 counted in `moves_root_kept` (or
  `moves_root_nearest`). A failed check fails the run; it never becomes a
  slow sample.
- **Reported beside each latency:** the root each target was checked to
  have and the medians of the path counters it moved (summary.csv's
  `target_root`, `root_distance` and `median_path_*`, and the latency
  tables), and, in the report's History window section and the manifest's
  `windows`, the window's settings and the roots the fixture branch keeps
  at each cell's start and end.

Merges are held to the same window: their base is the target checked
before them. A fast-forward is a move to the merged head, which keeps a
root: it must count `moves_root_kept` and write no rows
(`move_tables_written` and `apply_rows` 0). A three-way merge may write the
rows it changes, at most K (`apply_rows`), and no `apply_loads` (a table
filled from nothing). More is a full table write, and fails the run.

### Dirty working sets: xsmall, small, medium, large

Branch creation, checkout, and soft and hard reset each run with a clean source branch
and with four sizes of **pending, uncommitted version-control changes**. SQL
transactions have committed their UPDATEs, but no `dolt_commit` has been made.
These cases run at D=0 and every configured historical distance, on every table
size and historical change count K, in the latency and memory passes.
`drop_table` also runs with each dirty size, at D=0 only. The branch deletions
run clean: the dirty rows would be the session branch's, not the deleted one's.

A dirty `reset_hard` at D>0 times both costs at once: discarding the Q dirty
rows and moving the branch back D commits. To tell them apart, read the dirty
cell at D=0 (the discard alone) and the clean cell at D>0 (the move alone).

The default `dirty_sizes` configuration is:

```json
{
  "xsmall": {"fraction": 0.00001},
  "small": {"fraction": 0.0001},
  "medium": {"fraction": 0.01},
  "large": {"fraction": 0.1}
}
```

Fractions are rounded up to a whole row. For the 9,615,384-row / 10 GB table,
these are 97, 962, 96,154, and 961,539 dirty rows. To hold the dirty count fixed
while varying table size, use `{"rows": 10}`, `{"rows": 100}`, etc. instead
of `fraction`. Counts must fit every configured table; oversized values fail
configuration validation instead of being clamped. Set `"dirty_sizes": {}`
to disable the additional dirty VC cases. Older configurations that omit the
field inherit the four default profiles.

Four profiles are the fewest a dirty-row scaling fit takes (see "Results and
empirical complexity"):
across profiles, a fit holds the table size fixed and varies Q.  Across table
sizes, a fit holds each profile by its label, so a fraction profile's slope is
of its Q growing with N, and a fixed-count profile's of N alone.

Each dirty setup updates Q evenly spaced primary keys to revision H+1, independently
of the K keys changed by the committed history. Only the revision column changes;
the payload is retained. Raw samples record `working_set`, `dirty_rows`, and
`dirty_row_logical_bytes` (the logical footprint of affected rows, **not** bytes
actually rewritten or allocated). Reports keep every profile and actual Q separate
so clean and dirty observations cannot be averaged together.

The operation contracts are:

- **Branch creation:** create the new branch at the requested committed hash;
  the source keeps its pending edits and the new branch starts clean.
- **Checkout:** switch from the dirty source to the target branch's own clean
  working set; switching back must reveal the original pending edits. These are
  the SQL APIs' branch-local working sets, not CLI-style movement of pending
  changes between branches. No force/discard flag is used.
- **Soft reset:** move HEAD while retaining both the existing committed contents
  and the pending edits. The expected diff to the new HEAD accounts for the exact
  union/overlap of the historical K keys and dirty Q keys.

Dirty setup, validation, and hard-reset cleanup are outside timing and memory
sampling (in a `reset_hard` case the hard reset is the timed operation, and
the cleanup too). Setup uses point probes, avoiding a pre-timed VC diff that could fold
or materialize pending storage state. Post-operation validation checks values,
HEADs, and full diff cardinalities before restoring the clean starting state.
Large dirty sets can make setup/validation substantially longer than the timed
operation. Memory baseline includes the already-dirty working set; growth is
the additional resident memory during the operation.

Diff uses `SELECT count(*) FROM dolt_diff(...)` so the engine must exhaust the
row diff while network output remains one scalar. This is **row-diff enumeration
plus aggregation**, not time-to-first-row, rendered CLI output, or transmitting
all changed payloads. The fork's typed `NULL::bench_rows` API is adapted to
Dolt/Doltgres's table-name API. Dolt uses CALL for VC procedures; PostgreSQL
engines use SELECT.

Setup, validation, branch deletion, and restoration run outside measurements.
Branches are fresh names and are removed after each successful test, avoiding
branch-count growth. Each soft reset is checked for both the new HEAD and the
preserved working rows/diff. Checkout probes the restored values; diffs check
cardinality. Failures abort the run and retain scratch refs for investigation.
Baseline loading verifies count, total payload length, revision sum, and payload
samples. The fork additionally checks that the table really uses prolly.

Operation order and table-size order are seeded and shuffled. Engines run
serially in a seeded shuffled order, with no OS cache drop: these are **warm-cache
observations**, not cold-cache or fully randomized interleaved engine trials.
Use repeated runs with different seeds to assess between-run effects. The tool
does not claim to equalize engine cache algorithms or memory limits. PostgreSQL
uses 256 MB shared buffers with autovacuum disabled; durability remains enabled.
Dolt and Doltgres automatic GC is disabled in their server configs. Explicit setup
commits, Go GC, OS reclaim, and normal engine activity can still affect results.

## Memory, CPU, and I/O

There are five passes, each a separate set of measured observations:

| Pass | Measures | How |
|---|---|---|
| `timing` | latency | nothing else runs |
| `memory` | RSS, **PSS**, private and swap memory | `/proc/PID/smaps_rollup` sampled every `sample_interval_ms` |
| `cpu` | CPU time, minor and major faults | one snapshot before the operation, one after |
| `io` | kernel process read and write bytes | one snapshot before the operation, one after |
| `counters` | the engine's own work counters (prolly: `pg_stat_prolly`) | read before the operation and after, over the same SQL session |

Primary latency uses Go's monotonic clock around one request on a persistent
SQL connection, through full result consumption. It excludes client process
startup and Python IPC. Instrumented passes' timings include their snapshots'
interference and must not be used as primary latency comparisons; the `cpu` and
`io` passes sample nothing in between, so the memory pass's sampling thread
doesn't perturb their counters.

Every pass runs by default (`passes` in `config.json`). A run can select some
instead, and separate runs of one configuration can be merged into one report:

```sh
python3 "$B" run --passes timing          # results/STAMP-a
python3 "$B" run --passes memory          # results/STAMP-b
python3 "$B" run --passes cpu,io,counters # results/STAMP-c
python3 "$B" report results/STAMP-a results/STAMP-b results/STAMP-c
# -> results/merged-STAMP (or --out DIR), with report.md, report.html, CSVs
```

A merge refuses runs whose configurations differ beyond their passes, runs
measuring a pass twice, incomplete runs, and runs of different binaries.

On Linux, each instrumented pass reads the server and all its descendants,
including PostgreSQL backends:

- **memory:** RSS and PSS, before, peak and immediately after the operation.
  Summed RSS counts shared mappings multiple times; PSS apportions them and is
  the better measure of the server tree's resident footprint. Also peak growth
  above baseline, immediate retained growth (which can be negative), private
  resident memory, and swap. Raw timestamped samples are retained.
- **cpu** and **io:** CPU time, minor/major faults, and kernel process
  read/write-byte deltas. These include the whole server tree and asynchronous
  work. Counter deltas are withheld if the process set changes. CPU time has
  kernel clock-tick resolution; I/O counters do not include all buffered I/O or
  prove durability.

Results recorded before the `cpu` and `io` passes existed took those counters
from the memory pass; reports of them still show them.

### Engine work counters

The `counters` pass runs only for engines that keep work counters
(`Engine.counters()`; today the fork's prolly, whose `pg_stat_prolly` counts
tree nodes read and written, chunks appended, rows and change-log pages a
fold read, pages copied, rows applied, and more). Around each operation it
calls `pg_stat_force_next_flush()` and reads `pg_stat_prolly`: once after
the dirty-row setup and its probes, and once before validation and
restoration, so each sample counts the timed statement alone. The view is
per database and the benchmark is the database's only session (autovacuum
is off), so nothing else counts in it. Each sample keeps the counters the
operation moved (`counters` in `samples.jsonl`). The report gives each
counter's median and range per operation in **Engine work counters**, and
`summary.csv` has `median_`, `min_` and `max_counter_<name>`. Counts repeat
from run to run, so they explain prolly's latencies (for example, how many
rows a dirty checkout writes back) where timings alone can't. A counter that
went backwards during an operation (a statistics reset) fails the sample.
Dolt and Doltgres keep no counters: the pass isn't run for them, and their
planned sample count leaves it out.

### Run-order drift

A cell's repetitions are spread across the run, between the other cells
(each repetition shuffles the operation order). So a series that rises or
falls with run order means its cost depends on state earlier work left
behind, such as dead change-log entries piling up or caches filling, and
not only on the operation measured. The report tests every cell's latency
repetitions and each engine counter of its `counters` pass repetitions in
the order they ran: a two-sided Mann-Kendall trend test, exact without ties
and normal with the tie correction, flagged at p < 0.001 with at least five
samples (so not at the default single repetition: set `repetitions` to 5 or
more for it). Seven repetitions that rise every time are flagged (p = 0.0004);
seven with one pair out of order are not (p = 0.003). The bar is high because
a report tests hundreds of series. Flagged latencies are marked `[drift ↑]`
or `[drift ↓]` in their cells, and **Run-order drift** lists every flagged
series with its trend, p and first and last values (also in `drift.csv`
and `drift.json`). A counter that drifts is that state found directly: the
same operation should count the same.

Sampling peaks are lower bounds. Operations shorter than the configured interval,
or without a periodic sample, are flagged. A peak that rises and falls between
samples can be missed even in a longer operation. This measures **resident
footprint and growth**, not cumulative allocation volume or exact peak live heap.
Allocator caches and Go GC affect retained memory. OS filesystem cache is not
fully attributed to process PSS. A zero resident-growth measurement does not mean
zero allocation. Lowering the interval increases overhead; 1 ms is allowed.

## Results and empirical complexity

Results are retained in `ROOT/results/TIMESTAMP-ID/`:

- **`report.html` and `report.md`**: human-readable tables with Dolt, Doltgres,
  and PostgreSQL prolly side by side, grouped by table size, changed rows, and
  history length. Includes latency ranges, memory growth and total footprint,
  CPU/I/O counters, and scaling exponents with confidence intervals. Short memory
  observations, missing values, and inconclusive fits are labeled explicitly.
  HTML is a standalone file with section navigation and light/dark styling;
  no server, internet connection, or third-party libraries are needed.
- `manifest.json`: complete/failed status, full config, planned sample count,
  host information, engine order, binary/build provenance, and harness hashes.
- `samples.jsonl`: every warmup and measured result, exact SQL, validity, elapsed
  nanoseconds, expected diff size, and raw memory observations.
- `summary.csv`: side-by-side-sortable latency and memory summaries by engine,
  N, K, D, operation, and pass. `report.txt` explains the scope.
- `comparison.csv`: the same measurements pivoted into a column per engine/metric
  for direct comparisons in a spreadsheet or analysis script.
- `scaling.csv` / `scaling.json`: log-log power-law slopes with within-level
  bootstrap intervals, fit R², and explicit inconclusive reasons.
- `drift.csv` / `drift.json`: the series flagged for run-order drift (see
  "Run-order drift").
- Fixture manifests, effective PostgreSQL settings, server logs, and total data
  directory apparent/allocated bytes. Directory sizes include history and WAL;
  they are not per-table compressed sizes or per-operation space growth.

```sh
python3 "$B" report ~/dbs/benchmarks/results/TIMESTAMP-ID
```

Both human-readable formats are generated automatically by `run`. The `report`
command also adds them to existing complete runs without rerunning benchmarks.

Each fit varies one of N, K, D, or Q while holding the other recorded dimensions
fixed. At least four levels spanning 8x and five repetitions per level are
required (the default is one). Memory fits also require positive samples and no short-operation flags.
The default three dirty sizes provide descriptive comparisons; add a fourth
distinct size for a Q scaling fit. Fractional dirty sizes change Q with N, so
use fixed `rows` profiles for an N sweep with Q held constant. Q fits group by
actual count rather than profile name; other fits keep profile and Q fixed.
No successful report is produced for an incomplete run or missing planned samples.
An exponent near zero suggests flat growth over the tested range, near one linear
growth; it is **not proof of asymptotic Big-O**. Fixed protocol costs, cache
thresholds, and background work can hide algorithmic behavior. Bootstrap intervals
describe within-run sampling variability, not between-host/run uncertainty.

## Manual experiments and tests

```sh
python3 "$B" start --engines prolly
psql -h 127.0.0.1 -p 15433 -U postgres -d bench_n9615384
python3 "$B" stop --engines prolly
python3 "$B" status

python3 -m unittest discover -s src/tools/vc_bench -p 'test_*.py'
(cd src/tools/vc_bench/driver && go test ./...)
```

To see where a run's time goes, set `VC_BENCH_TRACE` to a file before `run`
(or `prepare`). Every statement the harness sends is appended to it with its UTC
time and how long it took, long id lists shortened to their count. Each case
marks the start of its setup (`==== CASE <op> D=<d> <working set>: setup`), of
its timed statement (`---- timed`) and of its validation (`---- validation`).
That tells a slow cell's untimed setup and checks apart from the operation
measured, and shows a statement that is still waiting: the last line is the one
before it.

```sh
VC_BENCH_TRACE=~/dbs/benchmarks/trace.log python3 "$B" run --engines prolly --passes timing
tail -f ~/dbs/benchmarks/trace.log
```

Use your own branch for manual writes, preserving the `fixture_*` branches and
saved baseline commits. Manually started servers are yours to stop. To remove
data, stop the servers and remove the chosen root yourself; the harness provides
no automatic destructive cleanup command.

## Adding or changing an engine

Everything engine-specific is in `engines.py`: one `Engine` per engine, with
its default config, build command and prerequisites, server start/stop (and
server config), connection, and the SQL of its version-control operations,
hashes, diffs, benchmark table and recorded settings. The runner, the fixture
loader and preflight call the engine's methods and never test its name, so a
setting that must match across engines (automatic GC, for one) or a new engine
is a change to that file alone.
