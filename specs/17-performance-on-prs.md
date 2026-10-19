# 17. S suite on every pull request: a performance gate that holds

**Status: proposed 2026-10-18, on `1603edcaa2`; decided by the user the
same day, as recommended (part 7), with part 2's facts checked on
`8faba578f9`. No dedicated host is registered, so the timing gate and
the nightly on it wait until one is (part 3.2); everything else is meant
for GitHub-hosted runners. Nothing here is built; tracked in #240.**
How Dolt, Doltgres and doltlite benchmark pull requests and whether they
block on the results; what our S suite and CI do today; and a design with
two layers, deterministic work counters on hosted runners, which gate,
and paired timing against the pull request's base, reported on hosted
runners and gated only on a dedicated host, with one sticky comment
reporting both.

## 1. How the others do it

### Dolt: one run, a 5% rule, a label

`.github/workflows/sysbench-performance.yaml`, `k8s-benchmark-latency.yaml`,
`pull-report.yaml`, `ci-check-performance.yaml`;
`.github/scripts/performance-benchmarking/`; the runner image's source in
`ld/go/benchmark` (at dolt `51cc4458b7`).

- **Trigger:** every pull request touching `go/**`, automatically; also
  `#benchmark` / `#benchmark-mini` comments from an allow-list of
  maintainers (`validate-commentor.sh`).
- **Where:** a Kubernetes job on a dedicated, tainted EKS node pool
  (7-CPU limit, `GOMAXPROCS=7`). The image builds the base and the head
  from source and runs them one after the other in the same pod.
- **What:** about 19 sysbench tests (reads, writes, `bulk_insert`), 100k
  rows, `--rand-seed=1`, 20 s each for the automatic run (120 s for
  `#benchmark`); a TPC-C job (1 thread, scale 1, 800 s) beside it.
- **Decision:** median latency (`--percentile=50`) per test; a regression
  if any test is more than 5% slower than base. **One run per version, no
  repetitions, no interleaving, no confidence interval.**
- **Gate:** the bot adds `performance_approved` when no regression is
  found and removes it otherwise; `ci-check-performance` fails until the
  label is present ("please allow ~25 mins"). A human can add the label to
  override. Quirks: the TPC-C job reports "no regression" too and can
  re-add a label the sysbench job removed; nothing removes the label on a
  new push.
- **Report:** one sticky comment per job (`<!-- go-run-output -->`):
  per-test from/to latency and percent change.
- **Nightly and release:** against MySQL 8.0.35, as per-test multipliers
  and read, write and overall mean multipliers; emailed, never gated.

### Doltgres: the same machinery, no gate

Every pull request runs the same mini sysbench on the same EKS pool
(`*_postgres` Lua scripts; TPC-C disabled as "not currently working") and
posts a sticky comment. No regression query, no label, no failing check.
Nightly and release compare with Postgres 15.5 (the README's table, "mean
multiple 2.7", is updated by hand).

### doltlite: paired, retried, and two kinds of gate

`.github/workflows/benchmark.yml`, `ci-build.yml`, `nightly-performance.yml`;
`test/benchmark_compare.py`, `benchmark_retry.py`,
`performance_counters.py`, `performance_hotspots.py`.

- **Trigger:** every pull request except README-only ones; part of the
  `CI Gate` job.
- **Where:** GitHub-hosted `ubuntu-latest`. One job builds optimized base
  (the pull request's base SHA, cached by a key over the SHA, scripts and
  toolchain, warmed every 6 hours) and candidate binaries; every
  measuring job runs both on the same VM, and checks the SHAs.
- **Timing:** wall clock per workload (sysbench-style, version-control
  operations, hotspot cases), 5 runs per arm (9 for autocommit writes),
  **alternating** base/candidate and candidate/base. Median per arm.
  Gates: a test fails at > 1.25x **and** > 10 ms slower; a section or
  suite at > 1.15x and > 10 ms; version-control tests at 1.50x and 50 ms.
  **Noise:** the whole suite is retried up to 3 times, and only a gate
  that failed on **every** attempt fails the pull request; others render
  as `TRANSIENT`.
- **Counters:** 15 engine counters from a `dolt_engine_stats` virtual table
  (cache hits and misses, chunk reads and writes, hashed and verified
  bytes, seeks, comparisons, ...), on reduced workloads. The candidate
  runs twice and must give identical counters. A counter regresses at
  ≥ 8 more **and** > 5% more than base. The only override is a reviewed
  line in the pull request's text: `perf-counter-exception: <workload>
  <counter> <reason>`.
- **Report:** sticky `<!-- benchmark:relative -->` (suite and per-test
  tables, ratios, PASS/FAIL/TRANSIENT, retries) and
  `<!-- benchmark:hotspots -->` (wall times, counter regressions, and a
  stock SQLite column, informational only).
- **Nightly:** against a verified stock SQLite (a past bug made the
  "stock" build use doltlite's storage, so every comparison passed; the
  build is now asserted). 55 paired runs; ceilings on doltlite/SQLite
  per workload (2.3x; 5.5x for autocommit writes) and per section (1.9x;
  5.0x), judged on the median of per-pair ratios. A failure opens or
  updates a labelled issue; a bot pull request refreshes
  `performance-report.md`. A nightly scale run has absolute wall-clock
  ceilings at 1M, 10M and 100M rows.

### What to take

| | Dolt | Doltgres | doltlite | Here |
|---|---|---|---|---|
| Base built beside head, same machine | yes | yes | yes | yes |
| Repetitions, interleaved | no | no | 5, alternating | yes: the runner already does it |
| Statistics | one sample, 5% | none | median, ratio and absolute floor, retry-confirmed | paired bootstrap CI (already in `stats.py`), retry-confirmed |
| Noise-free signal | none | none | engine counters, determinism-checked | WAL, I/O and engine counters, instructions |
| Gate | a label | none | the CI job | a required check |
| Override | add the label by hand | — | a reviewed line in the PR text | a reviewed line in the PR text |
| Against the reference engine | nightly, ungated | nightly, ungated | nightly ceilings | reported on PRs; nightly ceilings later |

## 2. What we have

From `specs/06-benchmarking.md` and `src/tools/prolly_bench`:

- **Workloads** S1–S10 and S13 built (TPC-B, point lookups, range scans,
  bulk load, a hot row, a held snapshot, multi-index, large transactions,
  TPC-C, sysbench, TPC-H); S11 and S12 planned.
- **Method:** fresh cluster per cell; paired cells across engines
  interleaved in seeded random order (`runner.py:344-368`); paired seeds
  per repetition; `table` gives per-metric medians and ratios to B0, and
  at benchmark quality a paired-bootstrap 95% interval (`stats.py:100-111`,
  2,000 resamples, ≥ 5 pairs), coloured only when it clears ±5%.
- **Counters already collected:** WAL bytes, records and full-page images
  (`collectors/wal.py`), `pg_stat_io` hits, reads, writes, extends,
  evictions and fsyncs, relation, database and cluster sizes including the
  chunk store (`space.py`). `engine_counters.py` reads `pg_stat_prolly`,
  which doesn't exist yet. The engine has counter structs that no view
  reads: the tree's node counters (`ProllyNodeCounters`,
  `prolly_tree.h`; a tree environment's `counters` pointer, which nothing
  sets today), the change finder's (`ProllyChangeCounters`,
  `prolly_changes.h`) and the value memo's (`ProllyMemoCounters`,
  `prolly_memo.h`).
- **Gates:** spec 06's gates 1–8 are all correctness; **no performance
  threshold exists anywhere in the specs.** `prolly_bench gate` validates
  evidence (complete cells, hashes, finite metrics, matching comparison
  identity), never ratios.
- **CI** (`prolly.yml`): `release-build` runs S1, S4, S9, S10, S13 at
  `integration.toml` (1 repetition, 2-second windows) on every pull
  request, and fails only on missing or invalid evidence; the numbers
  "say nothing about performance". `controlled-performance` runs S1 and
  S2 at benchmark quality on a self-hosted runner (`prolly-benchmark`),
  on manual dispatch only, informational.
- **Noise, measured** (`07-roadmap.md:119-128`, benchmark mode, 5
  repetitions, scale 5, 4 clients, 60 s): throughput CV 1.2%, latency
  CV 1.1–3.4%; without the benchmark host settings, CPU boost gave an
  8.4% outlier. Hosted runners are untested.
- **Intervals need five pairs.** `paired_ratio_interval()`
  (`stats.py:100-111`) returns nothing for fewer than 5 repetitions.
- **Pairing ignores builds.** The interleaving pairs cells by
  `CellKey.path()` (`interfaces.py:129-141`), which holds workload, size,
  variant, clients and repetition, not the engine or a build.
- **No dedicated host.** `prolly.yml`'s `controlled-performance` names a
  self-hosted `prolly-benchmark` runner, but none is registered (checked
  2026-10-18), and the repository has no Actions variables.
- **B0** is the fork's heap from the same build, not upstream Postgres.

So the comparison machinery is better than any of the three references',
but nothing compares a pull request with its base, nothing gates on it,
and nothing is posted.

## 3. Design

Two layers, as doltlite has, each gating on the pull request against its
base, with both engines (heap and `prolly`) measured: a regression in the
fork's heap matters too, since heap is the bar `prolly` is measured
against.

### 3.1 Layer 1: work counters (hosted, required)

What each workload *does*, counted, not timed: noise-free, so the gate
can be tight and run on free runners.

**Run shape** (`configs/s_counters.toml`): one client, a fixed amount of
work rather than a time window (`pgbench -t`, sysbench `--events`,
BenchBase with a fixed transaction count), fixed seeds, autovacuum off,
timed checkpoints off (`checkpoint_timeout` at its maximum, a large
`max_wal_size`, an explicit `CHECKPOINT` before the measured part), small
sizes that fit `shared_buffers`. Workloads: S1, S2, S3, S4, S7, S8,
S10 (a subset of its tests), S13 (Q1–Q22 serial). S5 (hot-row
contention) isn't among them: at one client it exercises none of the
lock module it is for, so it runs in layer 2, at 8 clients.

**Reading the counters.** Cumulative statistics are flushed by each
backend lazily (about once a second) and at its exit, so a reading taken
while the measured clients are connected can lag their work. The
measured part's clients disconnect before the counters are read, and the
collector waits until `pg_stat_activity` shows none of them, so every
backend that did the work has flushed. WAL bytes come from the LSN
difference, exact in any case. `pg_stat_io` is read for `client backend`
rows only (the collector sums every backend type today: filtering it is
new collector work, step 2), so the checkpointer's and background
writer's I/O, which timing decides, stays out.

**Counters, per unit of work** (per transaction, per row, per query;
the gate's floors apply to the totals over the fixed work, below):

| Counter | Source | Catches |
|---|---|---|
| WAL bytes, records, full-page images | `pg_stat_wal`, LSN difference | Logging more |
| Buffer hits, reads, writes, extends | `pg_stat_io`, client backends only | Touching more pages, plan changes |
| Relation, index and chunk-store bytes | `space.py` | Space amplification |
| Write amplification's logical, WAL and chunk-store totals (not its page total, which sums every backend type) | `write_amp.py` (`20-write-amplification.md`) | Writing more per byte of data |
| Instructions retired (user and kernel) | `perf stat -e instructions` over the measured part | CPU cost of any change |
| Engine counters: nodes read and written, chunks appended, bytes hashed, fold rows, value-tree chunks, commits, head moves, memo hits and misses, the change finder's counts | `pg_stat_prolly` (to build) | Prolly's own work |
| Rows, answers | the invariants, TPC-H digests | That both did the same work |

**`pg_stat_prolly`.** A built-in cumulative statistics kind, per
database and variable-numbered (one entry per database): backends
count in local memory and flush through pgstat, so the hot path costs
an increment. The view's shape is `(name, value)`, one row per counter, so
a counter is added without changing the view. The counters are #241's
list (nodes read and written, chunks appended and their bytes, bytes
hashed, fold rows read and written, value-tree chunks written, commits,
head moves), the value memo's hits and misses, and the change finder's
counts; the tree environment's `counters` pointer is set so trees count
into them. A variable-numbered kind isn't one `pg_stat_reset_shared()`
resets (that takes only fixed-numbered, cluster-wide kinds):
`pg_stat_reset()` resets them with the rest of the current database's
statistics, and `pg_stat_reset_prolly()` resets them alone, by resetting
the kind's entry for the current database.

**Instructions.** The collector probes for hardware counters (`perf stat
-e instructions:u,instructions:k`). Where there are none, as may be so
on hosted VMs, instructions leave the hosted gate, and Cachegrind's
counts (`valgrind --trace-children=yes`, the server's processes only, a
reduced workload) run nightly instead, reported.

**Determinism check.** Base and candidate each run twice. A counter
that differs between a build's own two runs is unstable there. Unstable
in the base, it is reported, not gated (a finding about the run shape).
Stable in the base and unstable in the candidate, it fails: the pull
request added something left to chance, and a regression must not hide
behind it (overridable like any regression). Instructions are the
exception: they vary a little, so they get a tolerance from the
calibration (part 3.5) instead of exact equality.

**Gate.** A counter regresses when the candidate is above base by more
than 5% **and**, in total over the run's fixed work, by more than a
floor per counter (8 events, 8 KB, or the calibrated instruction noise),
doltlite's rule. The floor is on totals so that a small per-transaction
increase over many transactions still counts, while a one-off (one more
extend in a run) doesn't. Fewer is never a failure; it's listed as an
improvement.

**Override.** An entry added by the pull request to
`src/tools/prolly_bench/configs/perf_accept.toml`, `<workload> <engine>
<counter|metric> <reason>`, accepts one regression. Only entries the
pull request's diff adds count (the file on the base is history), so an
override applies to the pull request that made it. Being in the diff,
it is reviewed like code: a new entry is a new commit, which re-runs the
gate and dismisses stale approvals where that is set. The comment lists
every accepted regression with its reason, so a reviewer sees what was
traded. A correctness fix that costs work is the expected use. The
file has a CODEOWNERS entry, which asks its owner for review; it
enforces nothing unless branch protection requires code owners'
review, which spec 16's settings don't (one maintainer, whose own pull
requests it would block). The check is the comment's list of accepted
regressions.

### 3.2 Layer 2: paired timing (hosted, reported)

**Builds.** One job builds the base and the candidate, both with
`build_postgres.sh` (`-O2`, no cassert, provenance recorded). The
candidate is the merge commit; the base is that merge commit's first
parent, not the event's `pull_request.base.sha`, which can be older
than the tip the merge
commit was made on, so the comparison holds exactly the pull request's
changes. Builds are cached by SHA and toolchain, and every push to any
branch builds and caches its head: a stack layer's base, the layer
below's head, is then cached by that layer's push, as `master`'s is by
its own. The candidate's harness drives both
binaries, so a change to a workload script changes both arms alike.

**Pairing by build.** `prolly_bench run S --pg-bin base=DIR,candidate=DIR
--am heap,prolly`: the runner's existing interleaving pairs cells across
builds as it does across engines (base and candidate alternate in seeded
random order, paired seeds per repetition), and `table` gives each
candidate cell's ratio to the same engine's base cell, with the paired
bootstrap interval. A build label joins `CellKey` (and its path), and
`compatible()` treats the build as the variable compared, requiring
everything else (machine, config, harness, clients) to match.

**Run shape** (`configs/s_pr.toml`): S1, S2 and S10 (read-write); 8
clients; 5 repetitions, the fewest that give an interval (part 2); 5 s
warm-up, 20 s measured. That is 3 workloads × 2 engines × 2 builds × 5
repetitions = 60 cells of 25 s, 25 minutes, plus a fresh cluster and
its load per cell (about 10 s each) and confirmation runs of the cells
that regress: about 40 minutes per pull request. Fewer workloads at five
repetitions, rather than more at three, which give no interval; S5 and
S9 run nightly. The nightly shape (S1–S4, S8, S10's three tests, S9; 1
and 8 clients; 5 repetitions; 10 s warm-up, 30 s measured: 360 cells,
about 4 hours) fits a hosted job's 6-hour limit. Metrics: throughput and
p50 latency; p99 is reported when it clears the sample floors.

**Decision.** A cell regresses when its whole 95% interval is beyond the
threshold in the worse direction; an interval that straddles it is
`inconclusive`, never a failure. Then doltlite's confirmation: cells that
regressed are run again (up to 3 attempts in all), and only a cell that
regressed on every attempt is `confirmed`; the rest are `transient`.

**Where it gates.** Hosted VMs share hardware, so hosted timing is
**reported, not gated** (user, 2026-10-18). A timing gate needs a
dedicated host, and none is registered; the parts below that need one
wait until the user provisions it:

- the timing gate itself, on the dedicated host, with the threshold at
  5%, the practical-effect threshold `table` already uses, on every pull
  request that changes code, queued by concurrency group, and required
  once the host is reliably available;
- the nightly's timing on it (part 3.6).

**When the dedicated host exists, it runs pull requests' code.** Today
`controlled-performance` runs only on `workflow_dispatch`. On pull
requests, its job runs without approval only for pull requests from this
repository's own branches; one from a fork waits for a maintainer,
through a protected environment (`benchmark`, with required reviewers).
The host keeps no secrets, runs the job as an unprivileged user, and
keeps its data under `PBENCH_DATA_ROOT`, which each job empties before
it runs.

**Durability.** Spec 06 holds `fsync` and `synchronous_commit` on for
every pairing, and reports no numbers with `fsync` off. The gate's
numbers are a comparison, not a report: hosted timing runs with
`synchronous_commit = off` (`fsync` on), because a hosted runner's disk
flush time is noise that would bury the change being measured, and the
cost of logging is caught exactly by layer 1's WAL counters; its numbers
are never reported as spec 06's. A dedicated host would run with both
on, as spec 06 requires.

### 3.3 Against heap, on every pull request (reported)

Each `prolly` row also shows `prolly`/heap from the same build, base and
candidate, so a pull request that narrows or widens the gap says so.
Not gated: spec 06 sets no target. Nightly ceilings on that ratio,
doltlite-style (median of per-pair ratios, per workload and per group),
are an open question until targets are decided.

### 3.4 The comment

One sticky comment, `<!-- s-suite -->`, updated in place:

```
## S suite: PASS (counters), timing reported
Base <sha> · candidate <merge sha> · counters and timing on ubuntu-24.04

### Counters (per transaction; a regression is > 5% and, in total, over its floor)
| Workload | Engine | Counter | Base | Candidate | Δ | Total Δ (10,000 txns) | Result |
| S1 | prolly | WAL bytes | 1,412 | 1,688 | +19.5% | +2.76 MB | FAIL |
| S3 | prolly | WAL bytes | ... | | | | accepted: "fix #210 logs the fold root" |

### Timing (median ratio, 95% CI; regression if the CI clears 5%)
| Workload | Clients | Engine | TPS ratio | p50 ratio | prolly/heap base → candidate | Result |
| S2 | 8 | prolly | 0.98 [0.96, 1.01] | 1.02 [0.99, 1.04] | 0.71 → 0.70 | inconclusive |

<details> unstable counters · transient cells and their attempts · improvements · environment </details>
Artifacts: <run link> · reproduce: prolly_bench run S --config ... --pg-bin base=...,candidate=...
```

`prolly_bench sreport` builds it from the counter and timing result sets
and the pull request's overrides.

### 3.5 Calibration first

Thresholds come from measurements, not from guesses. Before either
layer gates: A/A runs (the base against itself) on hosted runners, ten
times each, to measure each counter's stability, the instruction noise
(if the runners have hardware counters), and the timing intervals'
widths; on the dedicated host too once it exists. The target is a
false-failure rate under 1% per pull request across all gated cells;
the floors and thresholds above are adjusted to meet it, and the
measurements are recorded here. Until calibration is done, the **S
suite gate** runs and comments but isn't a required check.

### 3.6 Workflows

`.github/workflows/performance.yml`, on `pull_request`, `push` to
`master` and a nightly `schedule` (no `merge_group`: the repository has
no merge queue, spec 16). A push to any other branch runs only a
build-and-cache job (a workflow of its own, `performance-cache.yml`), so
a stack layer's base is cached without running the suite twice per
push:

1. **classify** (always runs; specs-only pull requests pass the checks
   without building, as in spec 16).
2. **build**: base (cached) and candidate, uploaded as one artifact with
   both SHAs, checked by every consumer. On `push` to `master`, it
   builds and caches the pushed head, then runs the suite against its
   parent.
3. **counters** (hosted, matrix by workload group): base, candidate,
   base again, candidate again; `prolly_bench sgate counters`.
4. **timing-hosted** (hosted): the timing layer, reported.
5. **S suite gate**: fails on a counter regression without an override,
   an invariant or evidence failure, or a missing job; uploads
   `comment.md`. A required check once calibration is done (part 3.5).
6. Later, with a dedicated host: **timing-dedicated**, the timing layer
   gated there (part 3.2), its confirmed regressions failing the gate.

The comment is posted by spec 16's comment workflow, which is generic
(parameterized by the comment's marker and the workflow it follows), on
`workflow_run` of this one.

**Nightly**: the 360-cell shape of part 3.2 (about 4 hours; not
`standard.toml`, whose full matrix would take about 100), at benchmark
quality, against the previous nightly's result set. Each night's result
set is kept as an artifact and summarized (medians, intervals, identity)
on an orphan `perf-history` branch. A confirmed regression opens or
updates one tracking issue labelled `nightly-performance-regression`,
and a nightly that fails before producing results opens a watchdog
issue. It runs on a hosted runner, reported with hosted noise, until a
dedicated host exists. Once the upstream baseline exists (the stock-heap
work), the nightly also reports the fork's heap against upstream
Postgres, the integration tax.

`prolly.yml`'s `release-build` keeps its evidence smoke (it checks the
harness, not performance); `controlled-performance` stays until a
dedicated host replaces it with `timing-dedicated`.

## 4. Why not the others' choices

- **One sample and a flat 5% (Dolt).** With about 19 tests at 5% each and
  no repetitions, noise alone fails pull requests, which teaches people to
  add the label by hand. The paired interval and retry confirmation make a
  failure mean something.
- **A label as the gate (Dolt).** It survives new pushes and can be
  re-added by a later job. The gate here is the check of the commit
  being merged.
- **No gate (Doltgres).** A comment nobody must read doesn't keep
  regressions out.
- **Timing alone on shared VMs (doltlite).** doltlite needs wide margins
  (25%, 10 ms) to live with hosted noise. Counters catch most regressions
  exactly, and leave timing to a quiet machine where 5% is meaningful.

## 5. Plan

| Step | Work | Done when | Issue |
|---|---|---|---|
| 1 | `pg_stat_prolly`: a built-in per-database pgstat kind, `(name, value)`, the counters of part 3.1, the tree environment's counters wired | `engine_counters.py` reports them; counts are exact on fixtures | #241 |
| 2 | Fixed-work run shape (`s_counters.toml`): `pgbench -t`, sysbench events, BenchBase counts, checkpoint control; the instruction collector, probing for hardware counters, Cachegrind nightly if there are none | Two runs of one build give identical counters (instructions within a measured tolerance) | #242 |
| 3 | `--pg-bin base=…,candidate=…`: builds as a pairing axis (a build label in `CellKey`, `compatible()`) in the runner and `table`; `s_pr.toml` at 5 repetitions | A/A gives ratios of 1 with intervals; a deliberately slowed candidate (a sleep in a hot path) is detected | #243 |
| 4 | Calibration (part 3.5) on hosted runners | Floors and thresholds recorded here, false-failure rate under 1% | #244 |
| 5 | `sgate counters`, `sgate timing` with retry confirmation, `perf_accept.toml` overrides (entries the diff adds) and its CODEOWNERS entry; `sreport` | Tests from fixtures: regression, improvement, accepted, unstable, transient, confirmed | #245 |
| 6 | `performance.yml`, reusing spec 16's comment workflow; build cache warmed on every branch's pushes | A pull request gets the comment; a deliberate counter regression fails the gate | #246 |
| 7 | **S suite gate** required once calibration is done (applied as spec 16's settings are) | A pull request with a counter regression can't merge without a `perf_accept.toml` entry | #246 |
| 8 | Nightly (the 360-cell shape) on a hosted runner, history on `perf-history`, the tracking and watchdog issues | A seeded regression on `master` opens the issue | #247 |
| — | With a dedicated host: its calibration, `timing-dedicated` and the `benchmark` environment for forks, the nightly moved there | A deliberate timing regression fails the dedicated check | later |

Steps 1 and 2 are also what spec 11's step 8 measures with, so they come
first.

## 6. Open questions

1. **Ceilings on `prolly`/heap.** Spec 06 sets no target. Recommended:
   none on pull requests; nightly ceilings once targets are set per
   workload.
2. **Hosted timing as a gate.** Decided: reported only. Calibration may
   show intervals narrow enough for a 10% threshold; that would be a
   new decision.

## 7. Decisions

Decided by the user 2026-10-18, as recommended:

1. **No dedicated host.** Everything is built for hosted runners; timing
   is reported, not gated; the parts that need a dedicated host wait
   until the user provisions one.
2. With it: `pg_stat_prolly` as a built-in per-database, variable-numbered
   pgstat kind with a `(name, value)` view, its counters #241's list plus
   the memo's hits and misses and the change finder's counts, the tree
   environment's counters wired, reset with the database's statistics
   or alone; the instruction collector probing
   for hardware counters and, without them, leaving instructions out of
   the hosted gate for a nightly Cachegrind run of the server's processes;
   the pull request's timing shape fewer workloads at five repetitions;
   pairing by a build label in `CellKey` and `compatible()`; history on
   an orphan `perf-history` branch and in artifacts; the **S suite gate**
   not required until calibration is done; #241 and #242 prerequisites
   of spec 11's step 8; one generic comment workflow, spec 16's, for
   both suites; `perf_accept.toml` under CODEOWNERS (a review request,
   not enforced); the nightly the
   360-cell shape, not `standard.toml`.

## 8. Calibration (part 3.5): measured on hosted runners

**Status: measured 2026-10-18 on GitHub's hosted runners (ubuntu-24.04),
#244.** The counter gate's settings below hold: no false failure in the
A/A data.  Timing stays reported, not gated (decision 1): ten runs can't
show a false-failure rate under 1%, and the intervals are wide.

**Hardware counters: none.** `perf stat -e instructions:u,instructions:k`
reports both `<not supported>` on every hosted runner probed (twenty,
over two calibration runs), so
instructions are out of the hosted gate, and the nightly counts them with
Cachegrind on a reduced workload, reported (part 3.1's fallback).

**Counters** (run 39155625916: 40 counter runs of one build, four on
each of ten runners, heap and prolly, `s_counters.toml` with S13):

- 385 of 430 exact counters (every `pg_stat_io` counter included) took
  the same value in every run.
- The rest jitter by a page or a few records: S7's (GiST's random
  tie-breaking), S10's read-write and update-index tests' extends,
  relation size and WAL (up to 25% of a handful of extends, about 1% of
  WAL), S13's hits (under 0.05%).  Under the original rule (any
  difference between a build's own two runs is instability, and
  instability only in the candidate fails), 1,809 of 2,000 simulated
  gates failed.  So a build's runs now disagree (unstable) only by what
  the rule would call a change, more than 5% and over the floor; smaller
  differences are jitter, gated as usual and listed.
- With that, no simulated gate (four distinct runs as base, base again,
  candidate, candidate again) failed: 0 of 240 within a runner, as a pull
  request's runs are, and 0 of 2,000 across runners; the same held for an
  earlier set of 40 runs (run 39153335247).  The floors stay 8 events and
  8 KB.
- A pull request's own run (#274, the candidate's engine code identical
  to its base's) passed the counter gate with 36 jittering counters listed.

**Timing** (the same run: ten A/A runs of `s_pr.toml`, 8 clients, 5
repetitions, 5 s warm-up, 20 s measured):

| | median 95% CI width | widest |
|---|---|---|
| throughput, heap | 2.6–7.2% | 31% |
| throughput, prolly | 3.0–6.2% | 17% |
| p50 latency | 3.5–9.3% | 28% |
| p99 latency | 4.5–12.6% | 46% |

No cell of any of the ten runs regressed at 5% or at 10%.  A 5%
threshold is narrower than many intervals: such cells are inconclusive,
not failures, and a 10% gate (open question 2) would need more runs
than ten to show it fails under 1% of pull requests.

**Tooling:**

- `.github/workflows/performance-calibration.yml` runs the A/A
  calibration on GitHub-hosted runners: one release build, then ten
  runners, each with four counter runs of that build
  (`s_counters.toml`) and one timing run of it against itself
  (`s_pr.toml`, `--pg-bin base=X,candidate=X`); it probes each runner
  for hardware instruction counters and records the answer. It runs by
  hand and on a pull request that changes it.
- `prolly_bench scalibrate` measures each counter's stability, the
  instruction noise, each timing cell's interval width and how often an
  A/A cell regresses at 5% and 10%, and the counter gate's false
  failures: the gate applied to four distinct runs as base, base again,
  candidate and candidate again (all choices, or a seeded sample of
  2,000). It suggests settings: an instruction tolerance twice the
  largest spread, and, if the gate fails A/A runs 1% of the time or more,
  floors twice the largest difference it failed on.
- `src/tools/prolly_bench/configs/s_gate.toml` holds the settings the
  gate uses: today relative 5%, floors of 8 events and 8 KB, instruction
  tolerance 1%, timing threshold 5% (reported only on hosted runners),
  3 attempts.

**Measured on a development machine** (not a hosted runner; 2026-10-18,
release builds, two counter runs of heap and prolly, S13 left out):
every counter of S1, S2, S3, S4, S8 and S10's three tests took the same
value in both runs (305 of 315 counters); S7's WAL, full-page-image,
extend and relation-size counters varied by under 4%, because GiST
breaks ties between equally good pages at random (`gistchoose()`), which
no setting seeds, so the gate reports them as unstable in the base.
Two causes of variation were removed: S7's update drew its new point from
the server's unseeded `random()` (it now uses pgbench's seed), and
`ANALYZE` sampled rows at random (the counter shape's statistics target
now makes it read every row). User-space instructions, counted with
`perf stat`, varied by at most 0.17% (S4), most cells by under 0.01%.
A timing A/A run on the same, busy machine (5 s windows) gave ratios of
0.93 to 1.02 with every interval containing 1.
