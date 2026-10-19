# 06. Benchmarking and comparison

## Purpose

Measure each pairing in `04-candidate-designs.md` and quantify what it costs
relative to stock Postgres (B0). There are two separate suites, with separate
harnesses, because Postgres's existing engines can't do version control:

1. **S suite (storage).** Ordinary database workloads that run on any table
   AM. B0 is the bar to meet.
2. **V suite (version control).** Branch, commit, diff, merge, clone/push.
   Prolly engines only; there is no B0 baseline. Engines are compared with
   each other (and with Dolt figures for orientation only).

## Correctness gates

Results are only trustworthy for an engine that behaves correctly, so every
results set records which of these pass.

**The requirements suite** (`src/test/prolly`, added 2026-10-13) encodes the
decided requirements as tests: Postgres semantics, concurrency and
durability (expected output generated from heap, so the suite also runs
against heap to validate itself), and version control and canonical form,
including gate 6. Run it with `make -C src/test/prolly check PROLLY_AM=<am>`;
stock Postgres's `make check` doesn't run it. Gates 1 and 2 are its
`check-upstream-regress` and `check-upstream-isolation` targets.

1. `make check` (regress) with `default_table_access_method` set to the prolly
   AM, with known, documented exceptions (e.g. tests that print `ctid`).
2. `src/test/isolation` with the same setting. This is the semantic proof of the
   concurrency design.
3. Recovery TAP tests (`src/test/recovery`), including crash-during-commit
   injection points.
4. The cross-engine **hash-equivalence test** (`05-storage-targets.md`), once
   more than one engine exists.
5. An amcheck-style consistency checker for the engine's on-disk structure.
6. **History independence** (`09-versioned-catalogs.md` part 1): the same
   logical state (schema and data) reached through different histories
   (different DDL orders, insert/delete churn, different clusters) gives
   identical canonical root hashes for their version-control commits (the
   commit ids differ: they also cover parents and message, as in Dolt).
7. **Complexity** (`10-complexity-requirements.md`, user requirement
   2026-10-14): every version-control operation is at worst as cheap as
   Dolt's, in time and space. The V suite sweeps N with Δ fixed and Δ with N
   fixed, and engine counters of nodes read and written are checked against
   each operation's bound, with timing reported beside them.
8. **The C suite** (`12-correctness-suite.md`, decided 2026-10-15):
   sqllogictest's 5.7M records that run on Postgres, compared record by
   record with heap's answers. The gate fails on a regression (heap
   passes, the engine doesn't, and not by row order alone), a hidden
   regression (both fail, differently), a crash, or a record only one side
   ran, outside the documented exceptions
   (`src/tools/prolly_bench/configs/c_exceptions.toml`). Run with
   `prolly_bench run C --am heap,<engine>`.

A failing gate is reported next to the numbers it affects.

## S suite workloads

| ID | Workload | Tool | Stresses |
|---|---|---|---|
| S1 | TPC-B-like read/write | `pgbench` default script | Commit path, WAL, group commit. **The first and most important test**: if the commit path can't keep up, nothing else matters |
| S2 | Read-only point lookups | `pgbench -S` | Cursor descent, read hints, cache |
| S3 | Range scans | Custom pgbench script | Leaf iteration, prefetch, decode cost |
| S4 | Bulk load | `COPY` of a large table, then index build | Chunker throughput, in-place vs copy (D3's expected win) |
| S5 | Hot-row contention | Custom script, few rows, many clients | Lock module (K1/K2/K3), waits, deadlock detection |
| S6 | Long snapshot under writes | One RR transaction held open while S1 runs | Old-version retention: pinned roots (D1–D3) vs in-page versions (D4), commit-marker growth (K2) |
| S7 | Secondary-index heavy | Tables with several btree indexes, plus one GiST | Index trees from the same root, content-derived TIDs for foreign AMs |
| S8 | Large transactions | Updates touching 1–10% of a table | Lock memory, mutation-map size, rebase cost |
| S9 | Mixed OLTP | BenchBase TPC-C (`~/cursor_src/benchbase` exists locally) | Overall realism |
| S10 | sysbench OLTP | sysbench 1.0 Lua tests, one variant each: `oltp_read_write`, `oltp_read_only`, `oltp_write_only`, `oltp_point_select`, `oltp_update_index`, `oltp_update_non_index`, `oltp_insert`, `select_random_ranges`, `bulk_insert` | Standard OLTP mixes; the tool Dolt and Doltgres publish latency comparisons with, so results can be set beside theirs (for orientation only) |
| S11 | Many tables | Many tables, small transactions touching a few | Root-layout directory cost: paths through the sub-root trees vs a flat catalog |
| S12 | Duplicate-heavy keyless tables | Tables without a primary key, many identical rows | Item counts, ANALYZE, per-duplicate TID metadata |
| S13 | Analytical queries | BenchBase TPC-H, Q1-Q22; complete serial pass and timed query mix | Joins, aggregates, subqueries and analytical scans; one stream because upstream Q15 uses a shared view |

S11 and S12 are planned workloads; S1-S10 and S13 are implemented in the
harness. TPC-H serial mode runs each query cold then hot and records the
hot execution, following BenchBase's untimed serial procedure.

Scale each workload at two sizes: fits in `shared_buffers`, and several times
larger than RAM.

## V suite workloads

| ID | Workload | Stresses |
|---|---|---|
| V1 | Branch create / checkout | Root install cost |
| V2 | Dolt-style commit after N row changes, N swept | Hashing cost (deferred-hashing engines D2–D4 pay here) |
| V3 | Diff and three-way merge between two branches with controlled change sets (size, locality, conflicts) | Structural sharing; canonicalization cost for D3/D4 |
| V4 | Clone / push / fetch between two local instances (over a `postgresql://` network remote) | Chunk export path |
| V5 | Sparse diff: one changed row in a large table | Work proportional to the change (subtree skipping), not to the table |
| V6 | Space and collection after deleting branches: branches changing rows, `dolt_gc()`, the branches deleted, `dolt_gc()` (space read), `dolt_gc('--full')` (space read), and `dolt_gc('--full')` again with everything live (#223) | Space returned, and a collection's cost against what changed and against the store |

Each V workload is also run with an S workload in the background (S1 by
default), to measure how version-control operations and ordinary traffic affect
each other.

## Metrics

| Metric | Source |
|---|---|
| Throughput (TPS) | pgbench / BenchBase |
| Latency p50 / p99 / p99.9 | pgbench `--log`, BenchBase |
| WAL bytes per transaction | `pg_stat_wal`, or `pg_current_wal_lsn()` difference / transactions |
| Write amplification: bytes written per logical byte, as WAL, data pages and (prolly) chunk store | The `write_amp` collector (`20-write-amplification.md`) |
| Device bytes written per transaction | `/proc/diskstats` before/after (the whole device); optionally the device's own counters |
| Space amplification | On-disk size / logical data size; allocation waste for D3/D4 by block size |
| Cache hit rate, buffer evictions | `pg_stat_io` |
| CPU per transaction, hashing share | `perf` profiles |
| Engine work: nodes read and written, chunks appended and their bytes, bytes hashed, fold rows, value-tree chunks, commits, head moves, memo hits and misses, the change finder's counts | `pg_stat_prolly`, a per-database view of `(name, value)` rows from the cumulative statistics system (`17-performance-on-prs.md`, decided 2026-10-18; not built) |
| Commit-pipeline queue depth and wait time | Engine counters in `pg_stat_prolly` |
| Lock/coordination memory | Engine counters in `pg_stat_prolly` |
| Recovery time | Time from crash to accepting connections after a fixed WAL volume |
| GC / vacuum cost | Time and I/O of the engine's reclamation pass |
| Bytes by structure | Canonical bytes (data and index trees), allocated pages (node occupancy), TID-map bytes, and WAL, reported separately; tree operations measured apart from SQL execution |
| Commit-lock hold time | Time a commit holds the branch's commit lock |
| Version-control op latency (V suite) | V1–V4 and V6 wall time |

## Environment

- The local development machine for now. Other storage types join the grid
  later.
- Record the device with `fio` baselines (4 KB random write, sequential write,
  fsync latency) alongside every run, so results can be normalized across
  machines.
- Same machine, same build flags (`-O2`, no assertions, no `--enable-cassert`)
  for every pairing. B0 is built from the same tree.
- Fixed CPU frequency governor; no other load; record kernel, filesystem and
  mount options.
- Postgres settings held constant across pairings except the ones under test
  (`shared_buffers`, `max_wal_size`, `checkpoint_timeout`,
  `synchronous_commit=on`, `fsync=on`).
- Real fsync always. Results with fsync off are not reported.

## Procedure

1. Initialize fresh, load, then `CHECKPOINT`.
2. Warm-up period (discarded).
3. Measurement period long enough to cover several checkpoints (D2's costs land
   at checkpoint).
4. At least 5 repetitions per cell; report median and spread (min/max or 95%
   CI). Treat differences inside the spread as no difference.
5. Client counts swept: 1, 4, 16, 64, 256.
6. Keep raw logs and the exact config for every run.

## Reporting and comparison

- S suite: one table per workload, rows = pairings plus B0, columns = metrics.
  Normalize to B0 (B0 = 1.0).
- V suite: one table per workload, rows = pairings, columns = metrics. No
  normalization.
- For each pairing, state which hypothesis in `04-candidate-designs.md` it
  confirms or refutes.
- Store results under `specs/results/<date>-<suite>-<pairing>/` with a short
  README per run.

## Harness

The harness is `src/tools/prolly_bench/` (its README has usage). Choices that
affect how results are read:

- **Fresh cluster per cell.** Every (workload, size, variant, clients,
  repetition) cell runs initdb, load, `CHECKPOINT`, warm-up, measurement. That
  is affordable for sizes that fit in memory; for the larger-than-RAM size,
  loading per cell is not (open question 18).
- **Table AM under test** reaches every table through
  `default_table_access_method`, including tables pgbench and BenchBase
  create. For `prolly`, whose tables exist only in versioned schemas, each
  benchmark database also gets a versioned schema first on its
  `search_path` (until 2026-10-15, `--am prolly` measured heap). Every cell
  checks its tables' AM. Indexes, including primary keys, are created with their default AM
  (btree); see open question 19.
- **CPU** from `/proc/stat` and perf is explicitly labeled host activity.
  It is not attributed to the server by subtracting client CPU. A server-only
  CPU figure requires a separate trustworthy accounting source.
- **Device bytes** come from `/proc/diskstats` for the device holding the data
  directory, so they include anything else writing to that device. The data
  directory is made copy-on-write-free on btrfs (`chattr +C`), which also
  turns off compression for it.
- **Intermediate output stays off the measured device.** Per-transaction
  latency logs, BenchBase samples and perf data are written to a per-cell
  tmpfs directory; only a latency histogram and small summaries are kept with
  the results. `latency_sampling_rate` (0.1 in the standard config) bounds
  how much pgbench logs in long, fast windows.
- **Process restart time** is measured before post-run maintenance: crash with
  `pg_ctl -m immediate`, restart, time to accepting connections. The WAL
  volume replayed (since the last redo point) is recorded with it rather than
  fixed in advance. Typed table-content digests must match before and after
  restart. The OS cache is warm; this does not simulate power loss.
- **Storage accounting** reports relation/database bytes separately from cluster
  apparent and allocated bytes including global engine stores and WAL. The old
  relation-size/row-representation ratio is not a valid D1 space amplification.
- **Evidence** uses a versioned pre-run plan and atomic completion manifest with
  artifact hashes. Reports and gates reject incomplete, corrupt, mistargeted or
  incompatible evidence. Paired cells are interleaved in seeded random order.
  Smoke ratios do not establish performance differences; benchmark-quality
  repetitions carry paired-bootstrap intervals and explicit metric scopes.
- **V workloads** take a "background" variant (`none` or `S1`); with `S1` the
  client count is the number of background pgbench clients.
- **Statuses.** `not_implemented` (SQLSTATE 0A000 from the engine, read from
  psql or the server log), `unavailable` (a tool is missing),
  `not_applicable`, `error`. Each says why.
- **System settings** for a run (governor, boost, perf permissions) are applied
  and reverted with `system/system_prepare.sh` and `system/system_restore.sh`;
  every result set records them, and the harness warns when they're not in
  benchmark mode.

