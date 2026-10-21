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
| Distance D from current HEAD | `history_depths` | 1, 4, 16, 32 commits |
| Repetitions | `repetitions`, `warmups` | 7 measured, 1 discarded per pass |
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
| `reset_soft` | Move a scratch branch's HEAD backward while retaining its working rows |
| `diff_equal` | Full count of row diff between identical commit hashes |
| `diff_working` | Full count of HEAD versus dirty WORKING |
| `diff_history` | Full count of old hash versus current hash |
| `diff_working_history` | Full count of old hash versus dirty WORKING |
| `resolve_ancestor` | Resolve `HEAD~D` without a diff |

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
Dolt automatic GC is disabled in the dedicated server config. Explicit setup
commits, Go GC, OS reclaim, and normal engine activity can still affect results.

## Memory, CPU, and I/O

There are separate `timing` and `memory` passes. Primary latency uses Go's
monotonic clock around one request on a persistent SQL connection, through full
result consumption. It excludes client process startup and Python IPC. Memory
pass timings include sampling interference and must not be used as primary
latency comparisons.

On Linux, the memory pass samples `/proc/PID/smaps_rollup` for the server and all
descendants, including PostgreSQL backends. It records:

- RSS and **PSS**, before/peak/immediately after the operation. Summed RSS counts
  shared mappings multiple times; PSS apportions them and is the better measure
  of the server tree's resident footprint.
- Peak growth above baseline, immediate retained growth (which can be negative),
  private resident memory, and swap. Raw timestamped samples are retained.
- CPU time, minor/major faults, and kernel process read/write-byte deltas.
  These include the whole server tree and asynchronous work. Counter deltas are
  withheld if the process set changes. CPU time has kernel clock-tick resolution;
  I/O counters do not include all buffered I/O or prove durability.

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
- Fixture manifests, effective PostgreSQL settings, server logs, and total data
  directory apparent/allocated bytes. Directory sizes include history and WAL;
  they are not per-table compressed sizes or per-operation space growth.

```sh
python3 "$B" report ~/dbs/benchmarks/results/TIMESTAMP-ID
```

Both human-readable formats are generated automatically by `run`. The `report`
command also adds them to existing complete runs without rerunning benchmarks.

Each fit varies one of N, K, or D while holding the other recorded dimensions
fixed. At least four levels spanning 8x and five repetitions per level are
required. Memory fits also require positive samples and no short-operation flags.
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

Use your own branch for manual writes, preserving the `fixture_*` branches and
saved baseline commits. Manually started servers are yours to stop. To remove
data, stop the servers and remove the chosen root yourself; the harness provides
no automatic destructive cleanup command.
