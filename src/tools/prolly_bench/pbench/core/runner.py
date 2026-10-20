"""The driver: runs one suite's grid of cells and hands results to sinks.

A cell is (workload, size, variant, client count, repetition).  Every cell
gets a fresh cluster: initdb, load, CHECKPOINT, warm-up, measured window
with collectors, post-measurement collectors.  The driver knows only the interfaces in interfaces.py.
"""

from __future__ import annotations

import datetime as dt
import gzip
import os
import shutil
import tempfile
import fcntl
import json
import re
import socket
import sys
import subprocess
import time
import traceback
import random
import dataclasses
from pathlib import Path
from typing import Any, Callable, Optional

from pbench.core import config as config_mod
from pbench.core import env as env_mod
from pbench.core import registry, targets
from pbench.core import correctness
from pbench.core.validation import sha256
from pbench.core.interfaces import (CellKey, CellResult, Cluster, Collector, Context,
                                    NotApplicable, NotImplementedByEngine, Outcome,
                                    RunInfo, Sink, Status, Unavailable, Window,
                                    Workload)
from pbench.core.sqlerr import FEATURE_NOT_SUPPORTED

# Server log lines carry the SQLSTATE (log_line_prefix '%m [%p] %e ').
_LOG_ERROR_RE = re.compile(r"\[\d+\] ([0-9A-Z]{5}) ERROR:\s+(.*)")


def _scratch_root() -> str:
    """tmpfs if available, so scratch output never touches a real device."""
    return "/dev/shm" if os.path.isdir("/dev/shm") else tempfile.gettempdir()


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("localhost", 0))
        return s.getsockname()[1]


class CollectorWindow(Window):
    """Starts collectors on entry and stops them (in reverse) on exit.
    A collector that fails is recorded and dropped; validation rejects its
    error after the remaining evidence has been collected."""

    def __init__(self, ctx: Context, collectors: list[Collector],
                 status: dict[str, tuple[str, str]]):
        self.ctx, self.collectors, self.status = ctx, collectors, status
        self.entered = self.exited = False
        self.seconds = 0.0
        self.timings = {}

    def _guard(self, c: Collector, fn: Callable[[], Any]) -> bool:
        start = time.monotonic()
        ok = guard(c.name, fn, self.status)
        self.timings.setdefault(c.name, []).append([start, time.monotonic()])
        return ok

    def __enter__(self):
        self.entered = True
        self.collectors = [c for c in self.collectors
                           if guard(c.name, lambda c=c: c.before_window(self.ctx), self.status)]
        self.collectors = [c for c in self.collectors
                           if self._guard(c, lambda c=c: c.start(self.ctx))]
        self.t0 = time.monotonic()
        return self

    def __exit__(self, *exc):
        self.seconds = time.monotonic() - self.t0
        self.collectors = [c for c in reversed(self.collectors)
                           if self._guard(c, lambda c=c: c.stop(self.ctx))][::-1]
        self.exited = True


def guard(name: str, fn: Callable[[], Any], status: dict[str, tuple[str, str]]) -> bool:
    """Run a collector hook; record why it couldn't run instead of failing."""
    try:
        fn()
        return True
    except Unavailable as e:
        status[name] = (Status.UNAVAILABLE.value, str(e))
    except NotApplicable as e:
        status[name] = (Status.NOT_APPLICABLE.value, str(e))
    except NotImplementedByEngine as e:
        status[name] = (Status.NOT_IMPLEMENTED.value, str(e))
    except Exception as e:  # noqa: BLE001 - a broken collector must not lose the cell
        status[name] = (Status.ERROR.value, f"{type(e).__name__}: {e}")
    return False


class Runner:
    def __init__(self, *, suite: str, cfg: dict[str, Any], table_am: str, label: str,
                 repo: Path, bin_dir: Path, work_dir: Path, out_root: Path,
                 command: str, keep_failed: bool = False, build: str = "",
                 builds: tuple[str, ...] = ()):
        registry.load_all()
        self.suite, self.cfg, self.table_am, self.label = suite, cfg, table_am, label
        self.repo, self.bin_dir, self.work_dir = repo, bin_dir, work_dir
        self.command, self.keep_failed = command, keep_failed
        # The build this runner's server is ("" unless builds are compared),
        # and every build compared, the base first.
        self.build, self.builds = build, tuple(builds)
        if suite == "V" and table_am not in targets.VERSIONED_HISTORY:
            raise SystemExit("The V suite measures version control, which only "
                             f"{', '.join(sorted(targets.VERSIONED_HISTORY))} has.")
        date = dt.date.today().isoformat()
        out = out_root / f"{date}-{suite}-{label}"
        n = 2
        while out.exists():
            out = out_root / f"{date}-{suite}-{label}-{n}"
            n += 1
        self.out_dir = out
        self.workloads = self._select_workloads()
        self.sinks: list[Sink] = [registry.sinks.get(s)() for s in cfg["run"]["sinks"]]

    def _select_workloads(self) -> list[Workload]:
        wanted = self.cfg["run"]["workloads"]
        found = []
        for name, cls in registry.workloads.items():
            if cls.suite != self.suite:
                continue
            if wanted and name not in wanted:
                continue
            found.append(cls())
        unknown = set(wanted) - {w.id for w in found}
        if unknown:
            raise SystemExit(f"unknown {self.suite} workloads: {sorted(unknown)}")
        return sorted(found, key=lambda w: int(w.id[1:]))

    def _make_cluster(self, instance: str) -> Cluster:
        cls = registry.clusters.get(self.cfg["run"]["cluster"])
        return cls(self.bin_dir, self.work_dir / self.out_dir.name, free_port(), instance)

    def run(self) -> list[CellResult]:
        return run_many([self])[0]

    def _open(self, lock: bool):
        if not self.work_dir.exists():
            self.work_dir.mkdir(parents=True)
            # On btrfs, turn off copy-on-write (and with it compression) for
            # everything created below; ignored on other filesystems.
            subprocess.run(["chattr", "+C", str(self.work_dir)],
                           capture_output=True)
        # One run at a time per work directory: concurrent runs would share
        # cluster directories, and would disturb each other's measurements.
        self._host_locked = False
        if lock:
            self._lock = open(self.work_dir / ".lock", "w")
            self._host_lock = open(Path(tempfile.gettempdir()) / f"prolly-bench-{os.getuid()}.lock", "w")
            try:
                fcntl.flock(self._lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                fcntl.flock(self._host_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise SystemExit("another prolly_bench run is active on this host for this user")
            self._host_locked = True
        self.out_dir.mkdir(parents=True)
        raw_root = self.out_dir / "raw"
        raw_root.mkdir()

        environment = env_mod.record(repo=self.repo, bin_dir=self.bin_dir,
                                     data_root=self.work_dir, tools=self.cfg["tools"])
        if self.cfg["run"]["fio_baseline"]:
            environment["fio"] = env_mod.fio_baseline(
                self.work_dir, int(self.cfg["run"]["fio_seconds"]), raw_root)
        warnings = env_mod.warnings(environment, self.suite)
        if (self.cfg["run"]["quality"] == "benchmark" or self.suite == "C") and warnings:
            raise RuntimeError("benchmark conditions failed: " + "; ".join(warnings))
        cells = list(self._cells())
        if not cells:
            raise RuntimeError("empty benchmark plan")
        random.Random(self.cfg["run"]["seed"]).shuffle(cells)
        info = RunInfo(suite=self.suite, label=self.label, table_am=self.table_am,
                       started=dt.datetime.now().isoformat(timespec="seconds"),
                       command=self.command, config=self.cfg, environment=environment,
                       warnings=warnings, out_dir=self.out_dir,
                       planned_cells=[key for key, _ in cells], build=self.build,
                       builds=list(self.builds))
        for s in self.sinks:
            s.open(info)

        self.info = info
        return cells

    def _cells(self):
        run = self.cfg["run"]
        for w in self.workloads:
            params = self.cfg["workloads"].get(w.id, {})
            clients = params.get("clients", run["clients"])
            for size in run["sizes"]:
                for vlabel, vparams in w.variants(params):
                    for c in w.client_counts(clients, vparams):
                        for rep in range(1, int(run["repetitions"]) + 1):
                            key = CellKey(self.suite, w.id, size, vlabel, c, rep,
                                          build=self.build)
                            merged = {"latency_sampling_rate":
                                      run["latency_sampling_rate"], "seed": run["seed"] + rep,
                                      "quality": run["quality"],
                                      "settled_counters": run["settled_counters"],
                                      "write_amp_materialize": run["write_amp_materialize"],
                                      **params, **vparams}
                            yield key, {"workload": w, "params": merged}

    # -- one cell ---------------------------------------------------------

    def _run_cell(self, key: CellKey, workload: Workload,
                  params: dict[str, Any]) -> CellResult:
        raw = self.out_dir / "raw" / key.path()
        raw.mkdir(parents=True)
        scratch = Path(tempfile.mkdtemp(prefix="pbench-cell-", dir=_scratch_root()))
        cluster = self._make_cluster("main-" + "-".join(key.path().parts))
        ctx = Context(key=key, cluster=cluster, table_am=self.table_am,
                      size_params=self.cfg["sizes"][key.size], params=params,
                      warmup_seconds=int(params.get("warmup_seconds",
                                                    self.cfg["run"]["warmup_seconds"])),
                      measure_seconds=int(params.get("measure_seconds",
                                                     self.cfg["run"]["measure_seconds"])),
                      raw_dir=raw, scratch_dir=scratch, tools=self.cfg["tools"],
                      make_cluster=self._started_cluster)
        result = CellResult(key=key, status=Status.OK, raw_dir=str(raw))
        t0 = time.monotonic()
        log_pos = 0
        failed = False
        try:
            for program, digest in self.info.environment["postgres"]["binary_hashes"].items():
                if sha256(self.bin_dir / program) != digest:
                    raise RuntimeError(f"{program} changed after the run was planned")
            if env_mod.build_provenance(self.bin_dir) != \
                    self.info.environment["postgres"].get("build_provenance"):
                raise RuntimeError("the build's provenance changed after the run was planned")
            cluster.create(config_mod.server_settings(self.cfg))
            cluster.start()
            cluster.sql(f"CREATE DATABASE {ctx.dbname}")
            target = targets.for_am(self.table_am)
            if target.setup_sql(ctx.dbname):
                cluster.sql(target.setup_sql(ctx.dbname), dbname=ctx.dbname)
            log_pos = cluster.log_position()
            target.verify(cluster, ctx.dbname)

            workload.check(ctx)
            collectors: list[Collector] = []
            for name in self.cfg["run"]["collectors"]:
                c = registry.collectors.get(name)()
                if guard(name, lambda c=c: c.check(ctx), result.collector_status):
                    collectors.append(c)

            workload.prepare(ctx)
            (raw / "pg_settings.json").write_text(cluster.scalar(
                "SELECT json_agg(s ORDER BY name) FROM (SELECT name, setting, unit, source "
                "FROM pg_settings) s", dbname=ctx.dbname) + "\n")
            ctx.tables = workload.tables(ctx)
            wrong = target.wrong_am(cluster, ctx.dbname, ctx.tables)
            if wrong:
                raise RuntimeError(f"tables not on {self.table_am}: {', '.join(wrong)}")
            cluster.sql("CHECKPOINT")
            correctness.verify(ctx, "before")
            workload.warmup(ctx)

            window = CollectorWindow(ctx, collectors, result.collector_status)
            outcome: Outcome = workload.measure(ctx, window)
            if not (window.entered and window.exited):
                raise RuntimeError(f"{workload.id} did not use the measurement window")
            result.metrics.update(outcome.metrics)
            result.metrics["window_seconds"] = window.seconds
            result.notes += outcome.notes
            result.read_only = outcome.read_only
            result.unit, result.denominator = outcome.unit, outcome.transactions
            result.measurement = {"scope": "client process including startup and shutdown",
                                  "collector_intervals_monotonic": window.timings,
                                  "start_monotonic": window.t0,
                                  "seconds": window.seconds,
                                  "seed": params["seed"],
                                  "offered_rate": params.get("rate"),
                                  "load_model": "scheduled" if params.get("rate") else "closed_loop",
                                  "cache_regime": "warmup; OS cache not flushed",
                                  "pg_stat_io_scope": "cumulative snapshots, may lag backend flushes",
                                  "device_scope": "whole data device including unrelated activity",
                                  "quality": self.cfg["run"]["quality"]}
            if not outcome.transactions or outcome.transactions <= 0:
                result.validation_errors.append("no completed work")
            wrong = target.wrong_am(cluster, ctx.dbname, ctx.tables)
            if wrong:
                result.validation_errors.append("post-run targeting failed: " + ", ".join(wrong))
            if outcome.unit != "transaction":
                result.notes.append(f"per-operation metrics are per {outcome.unit}")

            window.collectors = [c for c in window.collectors
                                 if guard(c.name, lambda c=c: c.after_window(ctx, outcome),
                                          result.collector_status)]
            for c in window.collectors:
                guard(c.name, lambda c=c: result.metrics.update(c.results(ctx, outcome)),
                      result.collector_status)
            correctness.verify(ctx, "after")
            workload.verify(ctx)
            for c in sorted(window.collectors, key=lambda c: c.post_order):
                guard(c.name, lambda c=c: result.metrics.update(c.post(ctx, outcome)),
                      result.collector_status)
            for name, (status, reason) in result.collector_status.items():
                if status == "error" or name in self.cfg["run"]["required_collectors"]:
                    result.validation_errors.append(f"collector {name}: {status}: {reason}")
        except NotImplementedByEngine as e:
            result.status, result.message = Status.NOT_IMPLEMENTED, str(e)
        except Unavailable as e:
            result.status, result.message = Status.UNAVAILABLE, str(e)
        except Exception as e:  # noqa: BLE001
            # A client may not report SQLSTATEs; the server log does.
            hit = self._not_implemented_in_log(cluster, log_pos)
            if hit:
                result.status, result.message = Status.NOT_IMPLEMENTED, hit
            else:
                failed = True
                result.status = Status.ERROR
                result.message = f"{type(e).__name__}: {e}"
                (raw / "traceback.txt").write_text(traceback.format_exc())
        finally:
            result.seconds = time.monotonic() - t0
            try:
                with gzip.open(raw / "server.log.gz", "wt") as f:
                    f.write(cluster.log_since(0))
            except OSError:
                pass
            shutil.rmtree(scratch, ignore_errors=True)
            for peer in ctx.peers:
                try:
                    peer.destroy()
                except Exception as e:
                    result.validation_errors.append(f"peer cluster cleanup failed: {e}")
            try:
                if (failed or result.validation_errors or result.status != Status.OK) and self.keep_failed:
                    cluster.stop("immediate")
                    result.notes.append(f"cluster kept at {cluster.data_dir}")
                else:
                    cluster.destroy()
            except Exception as e:
                result.validation_errors.append(f"cluster cleanup failed: {e}")
        return result

    def _started_cluster(self, instance: str) -> Cluster:
        c = self._make_cluster(instance)
        c.create(config_mod.server_settings(self.cfg))
        c.start()
        return c

    @staticmethod
    def _not_implemented_in_log(cluster: Cluster, pos: int) -> Optional[str]:
        for m in _LOG_ERROR_RE.finditer(cluster.log_since(pos)):
            if m.group(1) == FEATURE_NOT_SUPPORTED:
                return m.group(2).strip()
        return None


def write_json(path: Path, obj: Any) -> None:
    path.write_text(json.dumps(obj, indent=2, default=str) + "\n")


def interleave(plans, seed):
    """Group identical cells across engines and builds, randomizing the order
    of the groups and of the cells in each."""
    groups = {}
    for index, cells in enumerate(plans):
        for key, args in cells:
            groups.setdefault(str(key.pair_path()), []).append((index, key, args))
    rng = random.Random(seed)
    pairs = list(groups.values())
    rng.shuffle(pairs)
    for pair in pairs:
        rng.shuffle(pair)
    return [entry for pair in pairs for entry in pair]


class RunResults(list):
    """Cell results remain available even when derived reports fail."""

    def __init__(self, count):
        super().__init__([] for _ in range(count))
        self.report_errors: list[str] = []


def run_many(runners, report_format="both"):
    if not runners:
        raise ValueError("no engines requested")
    results = RunResults(len(runners))
    from pbench import report_files
    invocation = None
    execution_error = None

    def report_error(error):
        message = f"{type(error).__name__}: {error}"
        results.report_errors.append(message)
        print(f"Report generation failed; execution evidence retained: {message}", file=sys.stderr)

    try:
        plans = [runner._open(lock=i == 0) for i, runner in enumerate(runners)]
        if report_format != "none":
            try:
                invocation = report_files.begin(runners, report_format)
            except Exception as error:
                report_error(error)
        schedule = interleave(plans, runners[0].cfg["run"]["seed"])
        evidence = [{"engine": runners[i].table_am, "build": runners[i].build,
                     "key": dataclasses.asdict(key)}
                    for i, key, _ in schedule]
        for runner in runners:
            write_json(runner.out_dir / "raw/experiment-order.json", evidence)
        for order, (i, key, args) in enumerate(schedule):
            runner = runners[i]
            result = runner._run_cell(key, **args)
            result.measurement["experiment_order"] = order
            results[i].append(result)
            for sink in runner.sinks:
                sink.cell(result)
        errors = []
        for runner, cells in zip(runners, results):
            for sink in runner.sinks:
                try:
                    sink.close(runner.info, cells)
                except Exception as e:
                    errors.append(f"{runner.label}/{sink.name}: {e}")
        if errors:
            raise RuntimeError("evidence finalization failed: " + "; ".join(errors))
        return results
    except BaseException as e:
        execution_error = e
        for runner in runners:
            for sink in runner.sinks:
                try:
                    sink.abort(state="interrupted" if isinstance(e, KeyboardInterrupt) else "failed",
                               message=f"{type(e).__name__}: {e}")
                except Exception as cleanup_error:
                    print(f"Sink cleanup failed: {cleanup_error}", file=sys.stderr)
        raise
    finally:
        try:
            # A rejected invocation has no right to write into the active run's
            # report tree. Keep the lock through both independent report steps.
            if report_format != "none" and getattr(runners[0], "_host_locked", False):
                paths = []
                try:
                    if invocation is None:
                        invocation = report_files.begin(runners, report_format)
                    state = ("interrupted" if isinstance(execution_error, KeyboardInterrupt)
                             else "failed" if execution_error else "complete")
                    paths, _ = report_files.invocation(
                        invocation, execution=state,
                        execution_error=f"{type(execution_error).__name__}: {execution_error}"
                                        if execution_error else None)
                except Exception as error:
                    report_error(error)
                try:
                    paths += report_files.catalog(runners[0].out_dir.parent, report_format)
                except Exception as error:
                    report_error(error)
                for path in paths:
                    print(f"Report: {path}")
        finally:
            for runner in runners:
                for name in ("_host_lock", "_lock"):
                    lock = getattr(runner, name, None)
                    if lock is not None:
                        lock.close()
                runner._host_locked = False
