# 16. C suite results on every pull request, and a gate that holds

**Status: proposed 2026-10-18, on `64c9001f9a`; decided by the user the
same day, as recommended (part 7), with the facts of part 2 checked on
`8faba578f9`. Nothing here is built; tracked in #234.**
How Dolt, Doltgres and doltlite keep SQL-correctness regressions out of
their main branches; what our CI does today; and a design that posts the
C suite's results (spec 12) to every pull request and fails a pull request
that changes any record's outcome without saying so.

## 1. How the others do it

### Dolt: a percentage, a label and a bot commit

`.github/workflows/sql-regressions.yaml`, `k8s-sql-correctness.yaml`,
`pull-report.yaml`, `ci-check-correctness.yaml`;
`.github/scripts/sql-correctness/` (at `51cc4458b7`).

1. On a pull request touching `go/**`, a job reads the base branch's
   `current_correctness.txt` (one number: the pass percentage, now `100`)
   and sends a `repository_dispatch` with the PR number, branch and that
   number.
2. A second workflow creates a Kubernetes job on Dolthub's EKS cluster:
   a private image runs the sqllogictest corpus against the PR's build,
   writes results to S3, and compares the pass percentage with the
   number it was given (to 6 decimal places).
3. The job dispatches `pull-report`, which posts the results as a PR
   comment, adds the `correctness_approved` label if the percentage
   didn't drop (removes it if it did), and **commits the new percentage
   to the PR's branch** (`[skip actions] [ga-update-correctness]`).
4. `ci-check-correctness` fails until the label is present ("please allow
   ~25 mins"); that check is what blocks merging.

Strengths: the full corpus on every PR; a visible comment; the baseline
moves forward by itself. Weaknesses: one percentage hides a swap (one
record fixed, another broken); the bot writes to PR branches, and two PRs
racing both write the file; it needs private infrastructure (EKS, ECR,
S3, a token able to dispatch); the comment isn't sticky (a new one per
run).

### Doltgres: run main and the PR side by side, comment only

`.github/workflows/regression-tests.yml` (at doltgresql `HEAD`).

On every pull request, two jobs run Postgres's own regression suite
(replayed through Doltgres) on the PR merged with `main` and on `main`
itself; a third downloads both result trackers, renders regressions and
progressions with a Go tool, and posts one **sticky comment** (found by an
HTML marker, updated in place, truncated at 65,000 characters; skipped for
forks, whose token can't comment). It fails only if a run didn't finish
or deadlocked; **regressions don't fail CI**, they're for reviewers. The
sqllogictest corpus runs only at release, on the same Kubernetes job as
Dolt's (`k8s-sql-correctness.yaml`), with results by email.

Strengths: no stored baseline, so nothing to keep in sync; the
comparison is exact, record for record. Weakness: twice the compute, and
no gate.

### doltlite: a per-assertion allowlist, both ways

`.github/workflows/ci-build.yml`, `test.yml`;
`test/run_sqllogictest.sh`, `test/sqllogictest_gate.py`,
`test/known_sqllogictest_divergences.txt`.

Every pull request runs the full pinned corpus against doltlite and stock
SQLite (both built from the PR) and compares them assertion by
assertion. The allowlist names each known divergence by file and line,
and the gate is **bidirectional**: a divergence not on the list fails CI,
and a listed one that no longer diverges fails CI too ("list entries to
remove"), so the list stays exact. A canary first checks that the gate
can see a divergence at all. It posts no comment; the job's log is the
report.

Strengths: exact, cheap (one job), no infrastructure, an ordinary
required check. Weakness: nothing on the PR page beyond the check's
status.

## 2. What we have

`.github/workflows/prolly.yml`, job `build and test` (cassert build, a
4-CPU hosted runner), runs on every pull request except one that changes
only `specs/**`:

- `prolly_bench run C --am heap,prolly --config c_ci.toml`: C1, C2, and
  the first file of each directory of C3 and C4, four workers.
- `prolly_bench cgate`: fails unless every cell ran and every `prolly`
  record agrees with heap's, or both fail the corpus alike
  (`postgres_semantics`), or a listed exception covers it
  (`c_exceptions.toml`: per record, with a reason, an expiry and the
  answers' hashes; an exception that no longer differs fails). It is
  stricter than spec 12's text: `order_only`, `not_implemented` and
  timeouts fail it too.
- Nightly, the full corpus (`c_full_ci.toml`) runs **without the gate**.

Measured on the last runs (2026-10-18): the corpus checkout 80–90 s, the
smoke run and gate 190–340 s, the full corpus 2,300 s (38 minutes for
heap and `prolly`, one after the other, on 4 CPUs).

Gaps:

1. **No results on the pull request.** A reviewer sees a red or green
   check; the numbers are in a log and an artifact.
2. **The smoke run covers a small part of the corpus.** One file per
   directory is 28 of C3's 214 files and 4 of C4's 391 (C3 and C4 hold
   2.11M and 5.30M records). The full corpus is nightly, ungated, and its failure
   reaches no one.
3. **Heap is never checked against anything but itself.** The gate
   compares `prolly` with heap from the same build. A change to shared
   code (the planner, the executor, the fork's heap and catalog changes)
   that breaks both alike passes it: both answer the same wrong thing.
   Heap's own results against the corpus (99.787% pass, 12,113 records
   that Postgres answers differently from the corpus) are recorded in
   spec 12 but not checked.
4. **Nothing blocks a merge.** `master` has no branch protection
   (checked 2026-10-18): a failing check is advice. The repository is a
   personal account's (`coffeegoddd/postgres`), so it has no merge queue
   and no larger runners; it has no self-hosted runners and no Actions
   variables registered.
5. **A required check and `paths-ignore` don't mix.** If the workflow
   is skipped for a specs-only pull request, a required check from it
   never reports, and the pull request can't merge.
6. **`build and test` is flaky.** Its `test_prolly` run (and earlier the
   `vcscenario` adapters) has failed intermittently; the C suite hasn't.
   A flaky required check blocks merges for nothing.

## 3. Design

Three pieces: a **heap baseline** in the repository, checked both ways
(doltlite's model, applied to heap); the **existing engine gate**, on the
full corpus, sharded; and a **sticky comment** built from both
(Doltgres's model). No external infrastructure, no bot commits, no
second run of `master`.

### 3.1 The heap baseline

`src/tools/prolly_bench/configs/c_heap_baseline.tsv.gz`: one line per
record heap doesn't pass on the pinned corpus, sorted,

    file  line  status  sqlstate  sorted-answer-digest

(12,113 lines today, about 0.8 MB uncompressed). With every other record
`ok`, the list is heap's whole outcome on the corpus: a heap record that
starts failing appears in it, one that starts passing leaves it, and one
that fails differently changes its line.

`prolly_bench cbaseline check RESULTS` compares a heap result set with
the file, over the files the run covered (a sharded or sampled run checks
its part), and fails, doltlite-style, both ways:

- a heap record failing that the file doesn't list, or failing
  differently: **a regression in shared code**;
- a listed record that now passes: the file must be updated.

`prolly_bench cbaseline update RESULTS` rewrites the file from a full
run. A pull request that changes heap's answers updates the file in the
same commit, and the comment shows reviewers exactly which records moved.

Timeouts and records that didn't run are never written to the file; a
run that has any fails as incomplete, not as a regression, with its own
message, so a slow runner reads as a slow runner.

Bumping the corpus pin regenerates the file in the same pull request.

### 3.2 The engine gate, on the full corpus

The gate stays `cgate`, as it is: `prolly` against heap from the same
build, every class but `agree`, `postgres_semantics` and listed
exceptions failing. It runs on the full corpus on every pull request,
sharded so the wall clock fits:

- `prolly_bench run C --shard I/N`: files assigned to shards by size,
  largest first into the least-loaded shard (the runner already orders
  by size), deterministically from the pinned corpus; C1 and C2 go to
  shard 1. Each shard runs heap, then `prolly`, then `cgate` and
  `cbaseline check` on its part. `cgate` requires one cell per workload
  it is given, so each shard passes `--workloads` the workloads it holds
  files of (C1 and C2 only on shard 1; a workload with no files on a
  shard isn't gated there), and the **C suite gate** job checks that
  every workload was gated by some shard.
- **The union is the corpus.** `cgate` and the evidence checks compare a
  run with its own plan, not with the full corpus, so a shard that
  silently ran fewer files would pass. The **C suite gate** job checks
  that the shards' files, together, are exactly the pinned corpus's
  files, none twice.
- **The corpus pin** is in one file, read by the workflow's checkout
  and by `cbaseline`; today it is only the checkout ref in `prolly.yml`.
- Four shards: the full corpus's 38 minutes on one runner become about
  10 minutes per shard, plus build and setup; about 15 minutes per pull
  request in all. That is an estimate from the nightly's one-runner time,
  to be measured (#235). Over 15 minutes, the shards go to 6, then 8;
  still over, every push runs the smoke config and the full corpus runs
  on `ready_for_review` and on a label (part 7).

Engines: `heap` and `prolly` (D1 is removed, #183); a new engine joins
by its `--am` name.

### 3.3 The comment

One sticky comment per pull request, marked `<!-- c-suite -->`, updated
in place on every run, truncated with a link past 65,000 characters:

```
## C suite: sqllogictest @ 04b0d19, full corpus: PASS
Merge of <head> into master <base> · 5,675,180 records · 4 shards · 14 min

| | Pass (corpus) | vs base | agree | pg semantics | regression | hidden | order only | crashed | timeout | gate |
|---|---|---|---|---|---|---|---|---|---|---|
| heap   | 99.787% | ±0 | — | 12,113 | — | — | — | 0 | 0 | baseline: 0 new, 0 fixed |
| prolly | 99.787% | ±0 | 5,663,067 | 12,113 | 0 | 0 | 0 | 0 | 0 | pass |

<details><summary>New failures (0)</summary> ... </details>
<details><summary>Fixed (0): baseline entries to remove</summary> ... </details>
<details><summary>Stale exceptions (0)</summary> ... </details>
<details><summary>Time per cell</summary> heap / prolly, per workload </details>
Reproduce: prolly_bench cdiff <...> · artifacts: <run link>
```

Each new failure gives file and line, engine, both sides' status,
SQLSTATE and message, and the query (read from the corpus, shortened),
the first 25 by class. "vs base" counts against the baseline file at the
pull request's base commit (`git show <base>:…`), so a stack layer
compares with the layer below it, not with `master`.

`prolly_bench creport SHARD_RESULTS... --base-baseline FILE --out
comment.md` builds it from the shards' result sets, using the existing
`to_markdown` and `cdiff`.

### 3.4 Workflows

`.github/workflows/correctness.yml`, on `pull_request`, `push` to
`master` and a nightly `schedule` (no `merge_group`: there is no merge
queue). On `master`, the concurrency group is per commit
(`correctness-${{ github.sha }}`), never cancelling a run in progress:
a group per branch with `cancel-in-progress` would cancel one push's run
when the next push lands, and that run's failure would never be seen.
On pull requests, a new push cancels the older run.

1. **classify**: did anything outside `specs/**` change? (doltlite's
   job.) Always runs, so the required check always reports.
2. **build** (skipped for specs-only): the cassert build as
   `build and test` does it, plus `build_slt.sh`; uploads the install
   tree and the runner as an artifact. The corpus checkout is cached by
   its pinned commit.
3. **shard 1–4** (matrix): download, run heap and `prolly` on the shard,
   `cgate`, `cbaseline check`; upload the result sets.
4. **C suite gate** (the required check): always runs; passes for
   specs-only, otherwise downloads every shard, builds `comment.md` and
   uploads it, and fails if any shard failed or any shard is missing.

`.github/workflows/correctness-comment.yml`, on `workflow_run` of the
above: downloads `comment.md` and posts or updates the sticky comment,
with `pull-requests: write`, as `github-actions[bot]` through
`GITHUB_TOKEN` (no other token). It is generic, parameterized by the
comment's marker and the workflow it follows, and spec 17's performance
comment reuses it. A `workflow_run` workflow runs from the default
branch, so it can only be tested once it is on `master`. Commenting
from a separate workflow keeps write permission away from the pull
request's code, and works for forks.
Everything in the artifact was made by the pull request's code, so the
commenter trusts none of it but the comment's text: it takes the pull
request from the event (`workflow_run.pull_requests`, or, for a fork,
where that is empty, the open pull request whose head is the run's
`head_sha`, looked up through the API), posts nothing if it finds no
single one, and only edits a comment carrying the marker in that pull
request.

`prolly.yml` drops its two C steps and the corpus checkout, and, so
that **build and test** can become a required check, its `paths-ignore`:
it gets the same always-running **classify** job, and `build and test`
runs its steps only when something outside `specs/**` changed, reporting
success at once otherwise, so a specs-only pull request can merge.

**On `master`.** Every push runs the full suite. A failure (two pull
requests that each passed, merged in some order, or a flake) opens or
updates one tracking issue, labelled `ci-failure` and found by a marker
in its body, with the run's link and the report, as doltlite's
`nightly-scale.yml` does, and closes it on the next pass. With no merge
queue and no up-to-date rule, this issue is what catches `master`
drifting.

**Repository settings** (user, 2026-10-18).
- Protect `master`, requiring **C suite gate** only at first. **build
  and test** is required too once `test_prolly`'s flakes are fixed.
- No merge queue (a personal account's repository has none) and no rule
  that branches be up to date before merging (it would make every pull
  request rebase and rerun after each merge).
- Claude applies the settings with `gh api`, and only after asking the
  user at that time.

## 4. Why not the others' baselines

- **A percentage (Dolt).** It hides swaps, and our gate is already
  per record. Its bot commits to pull request branches race, and need a
  token that can push.
- **Running `master` beside every pull request (Doltgres).** Twice the
  compute, for what the baseline file gives exactly. With branches kept
  up to date, or a merge queue, every merge into `master` would be
  checked against the file as it merges; this repository has neither
  (part 3.4), so `master` can drift (two passing pull requests, merged in
  some order), and the tracking issue on `master` is what catches it; a
  flake is caught the same way.
- **Private infrastructure (Dolt, Doltgres's release run).** Hosted
  runners should finish the full corpus in about 15 minutes sharded (an
  estimate, part 3.2); Dolt's comment asks for 25.

## 5. Plan

| Step | Work | Done when | Issue |
|---|---|---|---|
| 1 | `run C --shard I/N`; measure 4, 6 and 8 shards on hosted runners; the corpus pin in one file | The full corpus in under 15 minutes of wall clock, with the shard count chosen, or the fallback of part 3.2 | #235 |
| 2 | `cbaseline check` and `update`; the first `c_heap_baseline.tsv.gz` from two full heap runs that agree | Heap against the file is clean; a test flips one record each way and the check fails with the right message | #236 |
| 3 | `creport`; the comment, from fixtures | Rendering tests for pass, regression, fixed, stale exceptions, incomplete shards and truncation | #237 |
| 4 | `correctness.yml` (the shard union check, per-commit concurrency on `master`) and the generic comment workflow; `classify` in `prolly.yml`; drop the C steps from `prolly.yml` | A pull request gets the comment (once the comment workflow is on `master`); a deliberately broken commit (an engine bug, and a shared-code bug that breaks heap too) fails the gate on a scratch pull request | #238 |
| 5 | Repository settings: protect `master`, requiring **C suite gate**; applied with `gh api` after asking the user | A failing pull request can't merge; a specs-only one can | #238 |
| 6 | The `master` tracking issue (`ci-failure`) | A failure on `master` opens it; the next pass closes it | #238 |
| 7 | `test_prolly`'s flakes fixed; **build and test** required | Its runs pass repeatedly; the setting applied as in step 5 | — |
| 8 | Spec 12's gate text and decision 5 brought up to date | Done | — |

## 6. Open questions

1. **Full corpus on every push, or smoke on every push and the full
   corpus before merge?** Decided (part 7): the full corpus on every
   push, with 4, then 6, then 8 shards to keep it under 15 minutes;
   otherwise smoke on push and the full corpus on `ready_for_review` and
   on a label.
2. **Should a fixed record fail CI?** Recommended: yes, as doltlite does
   and as stale exceptions already do: the file stays exact, and the
   comment says which entries to remove (or `cbaseline update` does it).
3. **cassert or release build for the C suite?** Recommended: cassert,
   as now: an assertion is a correctness failure the corpus can find,
   and the shards absorb the cost.
4. **Larger runners.** Hosted 16-CPU runners would run the full corpus
   in about 4 minutes unsharded (spec 12's timings), but a personal
   account's repository can't have them: sharding on standard runners
   it is.

## 7. Decisions

Decided by the user 2026-10-18, as recommended:

1. **Repository settings** (branch protection, required checks) are
   applied by Claude with `gh api`, only after asking the user for
   approval at that time.
2. **No merge queue and no required up-to-date branches**; a failure on
   `master` opens or updates a tracking issue (`ci-failure`).
3. **Required checks:** only **C suite gate** at first; **build and
   test** once `test_prolly`'s flakes are fixed.
4. With them: shards 4, then 6, then 8 if a pull request's run is over 15
   minutes, then smoke on push and the full corpus on
   `ready_for_review` or a label; the comment posted as
   `github-actions[bot]` through `GITHUB_TOKEN`, by a generic workflow
   spec 17 shares; the `ci-failure` label and a marker for the tracking
   issue; `classify` in `prolly.yml`; a per-commit concurrency group on
   `master`, never cancelling; the corpus pin in one file, read by the
   workflow and `cbaseline`; the gate job checking that the shards' files
   are exactly the corpus.
