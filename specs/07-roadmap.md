# 07. Roadmap

The roadmap is these six steps and ends at step 6. Nothing is planned beyond
it; what comes next is decided in step 6's discussion. Do the steps in order,
and don't start a step until a human asks for it.

## 1. Stub the interfaces for every engine

Make every planned interface exist and fail cleanly, before any of it works.

- Register table AMs for engines D1–D4 (`04-candidate-designs.md`) in the
  catalog (`pg_am`, handler functions in `pg_proc`): `prolly_d1`,
  `prolly_d2`, `prolly_d3a`, `prolly_d3b`, `prolly_d4`. D5 is not stubbed: it
  waits on the storage-target question.
- Each `TableAmRoutine` callback, and the prolly index AM's `IndexAmRoutine`
  callbacks, raise
  `ereport(ERROR, errcode(ERRCODE_FEATURE_NOT_SUPPORTED), …)` with a message
  naming the engine and the callback.
- Define **every interface** in `00-architecture-principles.md` as a header
  with its routine struct and written contract, a registry for selecting
  implementations by name, and a stub implementation of each. This includes the
  engine (D1–D4) and coordination (K1–K3) interfaces, so engines and modules can
  be paired as in `04-candidate-designs.md`.
- A conformance-test skeleton per interface that runs against every registered
  implementation (the stubs should fail it with "not implemented").
- Stub SQL functions for the version-control operations the V suite measures
  (branch create, checkout, commit, diff, merge, clone/push/fetch).
- Code lives under `src/backend/access/prolly/` and
  `src/include/access/prolly*.h`.

**Exit criteria:** the tree builds with no new warnings; `make check` passes
unchanged for heap; each stub raises its "not implemented" error when called
through SQL; every interface in `00-architecture-principles.md` has a header,
a stub and a conformance-test skeleton; the Postgres-facing adapters call only
interfaces.

**Status (2026-10-13): built, not yet reviewed.**

| Exit criterion | Result |
|---|---|
| Builds with no new warnings | Yes (autoconf build, gcc, `--enable-cassert`). The meson build files were written but not tried: meson isn't installed locally |
| `make check` passes unchanged for heap | 239/239. Four expected outputs gained rows for the new AMs and opclasses (`amutils`, `create_am`, `opr_sanity`, `psql`); no heap behaviour changed |
| Each stub raises "not implemented" through SQL | Yes for creating a table with each prolly AM (reaches the engine's `tree_create`), `SET ACCESS METHOD`, `CREATE INDEX … USING prolly`, and every version-control function. Table AM callbacks other than the storage ones can't be reached from SQL until a table can be created |
| Every C interface has a header, a stub and a conformance skeleton | Yes: 16 interfaces (the 15 planned plus diff/merge), `src/test/modules/test_prolly`. The suite also runs two test-only hash implementations to show it passes and fails correctly |
| Adapters call only interfaces | Yes: `prolly_tableam.c`, `prolly_indexam.c` and `prolly_vcs.c` go through `prolly_config.h` and `PROLLY_CALL` |

The benchmark workload, metric collector and result sink interfaces are part
of step 2.

## 2. Build the benchmark harnesses

Two separate suites, because Postgres's existing engines can't do version
control (`06-benchmarking.md`):

- **S suite (storage):** workloads S1–S9 that run on any table AM, so heap (B0)
  and every prolly engine are measured the same way.
- **V suite (version control):** workloads V1–V4 for prolly engines only.
  No heap baseline.

Both suites follow the same interface rule: workloads, metric collectors and
result sinks are each an interface with implementations, so adding a workload
or a metric doesn't change the driver.

Both suites:

- run with one command, parameterized by table AM (and pairing), scale and
  client count;
- set up a fresh cluster from a recorded config, then run, collect metrics and
  write results with the raw logs and the exact configuration;
- record the environment (`fio` device baseline, kernel, filesystem, CPU, build
  flags);
- report a stub's "not implemented" error as a clear "not implemented" result,
  not a harness failure.

**Exit criteria:** both suites run end to end; the S suite produces complete
results for heap; the V suite runs against the stubs and reports every
workload as not implemented.

**Status (2026-10-13): built, not yet reviewed.** Harness in
`src/tools/prolly_bench/` (usage in its README; reading notes in
`06-benchmarking.md`, Harness).

| Exit criterion | Result |
|---|---|
| Both suites run end to end | Yes. The quick check (`configs/smoke.toml`) takes about 2.5 minutes for S and 20 seconds for V |
| S suite complete for heap | 70/70 cells OK, S1–S10, every collector except perf (run with an earlier, larger smoke config) |
| V suite reports not implemented on the stubs | 8/8 cells `not_implemented` against `prolly_d1`. The V operations past setup (timed statements, background pgbench) were checked separately against a scratch cluster |

Result sets are kept locally under `specs/results/` and not committed.

Not yet exercised: the perf collector (needs `system/system_prepare.sh`),
and the larger-than-RAM size (open question 18). S10 (sysbench) was added
during step 2 at the user's request.

## 3. Demonstrate the harness and record a local baseline

- Run the S suite against stock heap (B0) on the local machine, at the
  repetition counts in `06-benchmarking.md`.
- Show that results are repeatable: the spread across repetitions is small
  enough to detect the differences we care about. If not, fix the environment or
  procedure before going further.
- Store the baseline under `specs/results/`.

**Exit criteria:** a reproducible local B0 baseline for S1–S9, with its spread
and environment recorded.

## 4. Implement the first engine

- The first pairing is **D1+K1** (proposed; confirm before starting, open
  question 13): immutable copy-on-write on Postgres pages, with a separate lock
  table. It's the simplest engine and the reference point for the others.
- Includes the shared foundation it needs (`04-candidate-designs.md`): node
  page format, chunker, content hashing, key and value encoding, canonical
  export, WAL resource manager, commit publish (B1, B2).
- Includes the version-control operations the V suite measures, to the scope
  agreed in open question 14.
- Every piece is an implementation of an interface from step 1; D1 and K1
  must pass their interfaces' conformance suites.

**Exit criteria:** the S and V suites run against D1+K1, and the correctness
gates in `06-benchmarking.md` are run, with their status recorded.

## 5. Measure the first engine

- Run both suites against D1+K1 on the same machine and configuration as the
  step 3 baseline.
- Collect profiles (`perf`) for the workloads where D1+K1 differs most from B0.

**Exit criteria:** S results side by side with B0 (normalized to B0 = 1.0), V
results, gate status, profiles; stored under `specs/results/`.

## 6. Discuss what we observe and why

- Write up what the numbers show and **why**, tracing each significant
  difference from B0 to a cause in the design or implementation, using the
  profiles and engine counters.
- State which hypotheses in `04-candidate-designs.md` the results support or
  contradict.
- Bring that to a discussion with the humans. The roadmap stops here; what to
  build next is decided then.
