# 17. S suite on every pull request: a performance gate that holds

**Status: proposed 2026-10-18, on `1603edcaa2`. Nothing here is built;
tracked in #240.**
How Dolt, Doltgres and doltlite benchmark pull requests and whether they
block on the results; what our S suite and CI do today; and a design that
gates every pull request on two layers, deterministic work counters on
hosted runners and paired timing against the pull request's base, with
one sticky comment reporting both.

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
  which doesn't exist yet.
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
sizes that fit `shared_buffers`. Workloads: S1, S2, S3, S4, S5, S7, S8,
S10 (a subset of its tests), S13 (Q1–Q22 serial).

**Counters, per unit of work** (per transaction, per row, per query):

| Counter | Source | Catches |
|---|---|---|
| WAL bytes, records, full-page images | `pg_stat_wal`, LSN difference | Logging more |
| Buffer hits, reads, writes, extends | `pg_stat_io`, client backends only | Touching more pages, plan changes |
| Relation, index and chunk-store bytes | `space.py` | Space amplification |
| Instructions retired (user and kernel) | `perf stat -e instructions` over the measured part | CPU cost of any change |
| Engine counters: nodes read and written, chunks appended, bytes hashed, fold rows, value-tree chunks | `pg_stat_prolly` (to build) | Prolly's own work |
| Rows, answers | the invariants, TPC-H digests | That both did the same work |

**Determinism check.** The candidate runs twice; a counter that differs
between its own two runs is unstable and is reported, not gated (and is
a finding: the run shape left something to chance). Instructions are
the exception: they vary a little, so they get a tolerance from the
calibration (part 3.5) instead of exact equality.

**Gate.** A counter regresses when the candidate is above base by more
than 5% **and** by more than a floor per counter (8 events, 8 KB, or the
calibrated instruction noise), doltlite's rule. Fewer is never a failure;
it's listed as an improvement.

**Override.** A line in the pull request's description,
`perf-accept: <workload> <counter|metric> <reason>`, outside code blocks,
read fresh through the API when the gate runs, accepts one regression; the
comment lists every accepted one with its reason, so a reviewer sees what
was traded. A correctness fix that costs work is the expected use.

### 3.2 Layer 2: paired timing (hosted for reporting; dedicated for the gate)

**Builds.** One job builds the base (the pull request's base SHA, cached
by SHA and toolchain, warmed on every push to `master`) and the
candidate (the merge commit), both with `build_postgres.sh` (`-O2`, no
cassert, provenance recorded). The candidate's harness drives both
binaries, so a change to a workload script changes both arms alike.

**Pairing by build.** `prolly_bench run S --pg-bin base=DIR,candidate=DIR
--am heap,prolly`: the runner's existing interleaving pairs cells across
builds as it does across engines (base and candidate alternate in seeded
random order, paired seeds per repetition), and `table` gives each
candidate cell's ratio to the same engine's base cell, with the paired
bootstrap interval.

**Run shape** (`configs/s_pr.toml`): S1, S2, S3, S4, S8, S10 (read-only,
write-only, read-write), S9 (TPC-C, 4 warehouses); 1 and 8 clients;
5 repetitions; 10 s warm-up, 30 s measured. Metrics gated: throughput and
p50 latency; p99 is reported when it clears the sample floors, never
gated on hosted runners.

**Decision.** A cell regresses when its whole 95% interval is beyond the
threshold in the worse direction; an interval that straddles it is
`inconclusive`, never a failure. Then doltlite's confirmation: cells that
regressed are run again (up to 3 attempts in all), and only a cell that
regressed on every attempt is `confirmed`; the rest are `transient`.

**Where it gates.** Hosted VMs share hardware; until calibration says
otherwise, hosted timing is **reported, not gated**, and the timing gate
runs on the dedicated host (`prolly-benchmark`, already in `prolly.yml`)
with the threshold at 5%, the practical-effect threshold `table` already
uses. The dedicated host runs the timing layer on every pull request
that changes code, queued by concurrency group, and its check is
required once the host is reliably available; until then it runs on the
merge queue (`merge_group`) only.

**Durability.** Spec 06 never reports numbers with `fsync` off. The gate's
numbers are a comparison, not a report: hosted timing runs with
`synchronous_commit = off` (`fsync` on), because a hosted runner's disk
flush time is noise that would bury the change being measured, and the
cost of logging is caught exactly by layer 1's WAL counters. The dedicated
host runs with both on, as spec 06 requires.

### 3.3 Against heap, on every pull request (reported)

Each `prolly` row also shows `prolly`/heap from the same build, base and
candidate, so a pull request that narrows or widens the gap says so.
Not gated: spec 06 sets no target. Nightly ceilings on that ratio,
doltlite-style (median of per-pair ratios, per workload and per group),
are an open question until targets are decided.

### 3.4 The comment

One sticky comment, `<!-- s-suite -->`, updated in place:

```
## S suite: PASS (counters), PASS (timing, dedicated host)
Base <sha> · candidate <merge sha> · counters on ubuntu-24.04 · timing on prolly-benchmark

### Counters (per transaction; a regression is > 5% and over its floor)
| Workload | Engine | Counter | Base | Candidate | Δ | Result |
| S1 | prolly | WAL bytes | 1,412 | 1,688 | +19.5% | FAIL |
| S1 | prolly | WAL bytes | ... | | | accepted: "fix #210 logs the fold root" |

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
layer gates: A/A runs (the base against itself) on hosted runners and on
the dedicated host, ten times each, to measure each counter's stability,
the instruction noise, and the timing intervals' widths. The target is
a false-failure rate under 1% per pull request across all gated cells;
the floors and thresholds above are adjusted to meet it, and the
measurements are recorded here.

### 3.6 Workflows

`.github/workflows/performance.yml`, on `pull_request`, `merge_group`,
`push` to `master` and a nightly `schedule`:

1. **classify** (always runs; specs-only pull requests pass the checks
   without building, as in spec 16).
2. **build**: base (cached) and candidate, uploaded as one artifact with
   both SHAs, checked by every consumer.
3. **counters** (hosted, matrix by workload group): base, candidate,
   candidate again; `prolly_bench sgate counters`.
4. **timing-hosted** (hosted): the timing layer, reported.
5. **timing-dedicated** (`prolly-benchmark`): the timing layer, gated
   (part 3.2).
6. **S suite gate**: the required check; fails on a confirmed timing
   regression, a counter regression without an override, an invariant or
   evidence failure, or a missing job; uploads `comment.md`.

`performance-comment.yml` on `workflow_run` posts the sticky comment, as
spec 16's comment workflow does (one comment workflow may serve both).

**Nightly** on the dedicated host: the full S suite at
`standard.toml`, benchmark quality, against the previous nightly's
result set (kept as an artifact and in a `perf-history` branch); a
confirmed regression opens or updates one tracking issue labelled
`nightly-performance-regression`. Once the upstream baseline exists (the
stock-heap work), the nightly also reports the fork's heap against
upstream Postgres, the integration tax.

`prolly.yml`'s `release-build` keeps its evidence smoke (it checks the
harness, not performance); `controlled-performance` is replaced by
`timing-dedicated`.

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
| 1 | `pg_stat_prolly`: the engine's counters (nodes read and written, chunks appended, bytes hashed, fold rows, value-tree chunks) | `engine_counters.py` reports them; counts are exact on fixtures | #241 |
| 2 | Fixed-work run shape (`s_counters.toml`): `pgbench -t`, sysbench events, BenchBase counts, checkpoint control; the instruction collector | Two runs of one build give identical counters (instructions within a measured tolerance) | #242 |
| 3 | `--pg-bin base=…,candidate=…`: builds as a pairing axis in the runner and `table` | A/A gives ratios of 1 with intervals; a deliberately slowed candidate (a sleep in a hot path) is detected | #243 |
| 4 | Calibration (part 3.5) on hosted runners and the dedicated host | Floors and thresholds recorded here, false-failure rate under 1% | #244 |
| 5 | `sgate counters`, `sgate timing` with retry confirmation, `perf-accept` overrides; `sreport` | Tests from fixtures: regression, improvement, accepted, unstable, transient, confirmed | #245 |
| 6 | `performance.yml`, `performance-comment.yml`; base build cache warmed on `master` | A pull request gets the comment; a deliberate regression in each layer fails the gate | #246 |
| 7 | Required checks (with spec 16's branch protection); the dedicated host's timing check required once it is reliable | A pull request with a confirmed regression can't merge without a `perf-accept` line | #246 |
| 8 | Nightly on the dedicated host, history and the tracking issue | A seeded regression on `master` opens the issue | #247 |

## 6. Open questions

1. **Instructions on hosted runners.** GitHub's hosted VMs may not expose
   hardware counters. Fallback: Cachegrind's instruction counts on a
   reduced workload under `valgrind --trace-children=yes` (exact, about
   50 times slower), as SQLite's own speed tests do. Step 2 finds out.
2. **Hosted timing as a gate.** Recommended: reported only, unless
   calibration shows intervals narrow enough for a 10% threshold.
3. **The dedicated host on every pull request, or the merge queue only.**
   Recommended: every pull request that changes code, if the queue keeps
   up (about 25 minutes per pull request at the run shape above);
   otherwise the merge queue.
4. **Ceilings on `prolly`/heap.** Spec 06 sets no target. Recommended:
   none on pull requests; nightly ceilings once targets are set per
   workload.
5. **Who may write `perf-accept`.** Recommended: anyone, since the line is
   in the reviewed pull request and the comment shows it; branch
   protection's required review is the check.
