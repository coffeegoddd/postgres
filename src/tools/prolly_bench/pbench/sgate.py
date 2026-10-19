"""The S suite's gate on a pull request: its counters and its timing against
its base's.

  prolly_bench sgate counters BASE BASE2 CANDIDATE CANDIDATE2 [--accept ...]
  prolly_bench sgate timing RUN [RETRY ...] [--threshold 0.05] [--report-only]

Each counter run is a directory of result sets of configs/s_counters.toml
(one per engine), the base's and the candidate's each run twice:

- A counter that differs between a build's own two runs by what the rule
  would call a change is unstable.  Unstable in the base, it is reported
  and not gated (a finding about the run shape); stable in the base and
  unstable in the candidate, it fails: the pull request left something to
  chance.  One that differs by less (jitter: hosted runners extend a page
  more now and then) is gated as usual and listed.
- A counter regresses when the candidate's is more than 5% above the
  base's and, in total over the run's fixed work, above its floor (8
  events, 8 KB, or the calibrated instruction noise).  Instructions are
  compared within a tolerance rather than exactly.
- A lower counter never fails; it is listed as an improvement.
- Missing or failed cells, and cells whose work differs (units of work,
  TPC-H answers), always fail: the two didn't do the same work.

An entry the pull request adds to configs/perf_accept.toml accepts one
regression (or one unstable candidate counter); entries already on the base
are history and accept nothing.

A timing run is one of configs/s_pr.toml with --pg-bin base=...,candidate=...
A cell regresses when its whole 95% interval lies beyond the threshold in
the worse direction, and is inconclusive when the interval straddles it.
Cells that regress are run again (RETRY runs, of those cells only), up to 3
attempts in all; a cell that regresses in every attempt is confirmed, the
rest are transient.  With --report-only (hosted runners), a regression
never fails the gate; a run whose evidence fails (cells missing or invalid,
no comparison of two builds) always does.

Each writes its findings as JSON (--json) for sreport, and exits 1 when
the gate fails.
"""

from __future__ import annotations

import json
import math
import sys
import tomllib
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Optional

from pbench import bcompare, scounters, table
from pbench.core.validation import cell_problems, compatible

thresholds = scounters.thresholds


# ---------------------------------------------------------------------------
# Accepted regressions
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Acceptance:
    workload: str
    engine: str
    counter: str               # a counter or a timing metric
    reason: str

    def matches(self, workload: str, engine: str, counter: str) -> bool:
        return (self.workload, self.engine, self.counter) == (workload, engine, counter)


def load_accept(path: Optional[Path]) -> list[Acceptance]:
    """perf_accept.toml's entries: [[accept]] workload, engine, counter,
    reason (each required, nothing else)."""
    if not path or not path.exists():
        return []
    with open(path, "rb") as f:
        raw = tomllib.load(f)
    if set(raw) - {"accept"}:
        raise ValueError(f"{path}: only [[accept]] entries belong here")
    out = []
    for e in raw.get("accept", []):
        if set(e) != {"workload", "engine", "counter", "reason"} or \
                not all(isinstance(v, str) and v.strip() for v in e.values()):
            raise ValueError(f"{path}: an entry needs workload, engine, counter and a reason: {e}")
        out.append(Acceptance(e["workload"], e["engine"], e["counter"], e["reason"].strip()))
    return out


def added(head: list[Acceptance], base: list[Acceptance]) -> list[Acceptance]:
    """The entries the pull request adds: on its head and not its base (the
    same regression, reason aside)."""
    old = {(a.workload, a.engine, a.counter) for a in base}
    return [a for a in head if (a.workload, a.engine, a.counter) not in old]


# ---------------------------------------------------------------------------
# Counters
# ---------------------------------------------------------------------------

@dataclass
class Finding:
    workload: str              # with its variant, "S10/oltp_read_write"
    engine: str
    counter: str
    result: str                # regression, improvement, unstable_base,
                               # unstable_candidate, jitter, accepted,
                               # invariant
    base: Optional[float] = None
    candidate: Optional[float] = None
    units: Optional[float] = None
    unit: str = "transaction"
    detail: str = ""
    reason: str = ""           # an accepted one's

    @property
    def fails(self) -> bool:
        return self.result in ("regression", "unstable_candidate", "invariant")


def _workload(cid: scounters.CellId) -> str:
    return f"{cid.workload}/{cid.variant}" if cid.variant else cid.workload


def run_sets(path: Path) -> dict[str, Path]:
    """{engine: result set} of a run's directory (or of one result set)."""
    candidates = [path] if (path / "run.json").exists() else sorted(
        p for p in path.iterdir() if (p / "run.json").exists())
    out = {}
    for p in candidates:
        engine = json.loads((p / "run.json").read_text())["table_am"]
        if engine in out:
            raise ValueError(f"{path}: two result sets of {engine}")
        out[engine] = p
    if not out:
        raise ValueError(f"{path}: no result sets")
    return out


def _invariants(label: str, sets: dict[str, table.ResultSet]) -> list[Finding]:
    out = []
    for engine, rs in sets.items():
        for problem in rs.problems:
            out.append(Finding("-", engine, "evidence", "invariant", detail=f"{label}: {problem}"))
        for r in rs.results:
            for problem in cell_problems(r):
                k = r["key"]
                w = f"{k['workload']}/{k['variant']}" if k.get("variant") else k["workload"]
                out.append(Finding(w, engine, "cell", "invariant", detail=f"{label}: {problem}"))
    return out


def _collectors_lost(loaded: dict, engine: str) -> list[Finding]:
    """A collector that ran in a cell of both base runs and not in that cell
    of a candidate run: its counters would otherwise just go missing, and a
    counter only one build has is skipped as nothing to compare."""
    def statuses(rs):
        return {scounters.CellId(r["key"]["workload"], r["key"].get("variant", ""),
                                 r["key"]["size"], r["key"]["clients"], engine):
                r.get("collector_status", {}) for r in rs.results if r.get("status") == "ok"}
    s = {n: statuses(sets[engine]) for n, sets in loaded.items()}
    out = []
    for cid in s["base"]:
        if cid not in s["base2"]:
            continue
        for run in ("candidate", "candidate2"):
            for name, (status, reason) in sorted(s[run].get(cid, {}).items()):
                if name in s["base"][cid] or name in s["base2"][cid]:
                    continue        # it didn't run in the base either
                out.append(Finding(_workload(cid), engine, f"collector {name}", "invariant",
                                   detail=f"ran in the base, {status} in the {run}: {reason}"))
    return out


def gate_counters(base: Path, base2: Path, cand: Path, cand2: Path,
                  accept: list[Acceptance], t: dict) -> list[Finding]:
    runs = {name: run_sets(p) for name, p in
            (("base", base), ("base2", base2), ("candidate", cand), ("candidate2", cand2))}
    loaded = {name: {e: table.load_set(p) for e, p in sets.items()}
              for name, sets in runs.items()}
    findings = []
    for name, sets in loaded.items():
        findings += _invariants(name, sets)
    engines = sorted(set().union(*runs.values()), key=lambda e: e != "heap")
    for engine in engines:
        missing = [n for n, s in runs.items() if engine not in s]
        if missing:
            findings.append(Finding("-", engine, "result set", "invariant",
                                    detail=f"no {engine} results in {', '.join(missing)}"))
            continue
        problems = compatible(runs["base"][engine], runs["candidate"][engine], "engine")
        problems = [p for p in problems if "different builds" not in p]
        for p in problems:
            findings.append(Finding("-", engine, "comparison", "invariant", detail=p))
        c = {n: scounters.counters(s[engine].results, engine) for n, s in loaded.items()}
        findings += _collectors_lost(loaded, engine)
        in_base = scounters.instability(c["base"], c["base2"], t)
        in_cand = scounters.instability(c["candidate"], c["candidate2"], t)
        for cid in sorted(set(c["base"]) | set(c["candidate"]), key=str):
            b, k = c["base"].get(cid), c["candidate"].get(cid)
            w = _workload(cid)
            if b is None or k is None:
                findings.append(Finding(w, engine, "cell", "invariant",
                                        detail="missing from the " + ("base" if b is None else "candidate")))
                continue
            # (the same work in all four runs: each build's two, and the builds)
            amounts = {n: c[n][cid].units if cid in c[n] else None for n in c}
            if len(set(amounts.values())) > 1:
                findings.append(Finding(w, engine, "units of work", "invariant", b.units, k.units,
                                        detail="the runs did different amounts of work: " +
                                        ", ".join(f"{n} {u}" for n, u in amounts.items())))
                continue
            for m in sorted(set(b.counters) | set(k.counters)):
                x, y = b.counters.get(m), k.counters.get(m)
                f = Finding(w, engine, m, "", x, y, k.units, k.unit)
                if in_base.get((cid, m)) == "unstable":
                    f.result = "unstable_base"
                elif in_cand.get((cid, m)) == "unstable":
                    f.result, f.detail = "unstable_candidate", "differs between the candidate's two runs"
                elif x is None or y is None:
                    continue           # a counter one build has: nothing to compare
                else:
                    f.result = scounters.change(m, x, y, t)
                    if not f.result and ((cid, m) in in_base or (cid, m) in in_cand):
                        f.result = "jitter"   # differs within a build, by less than a change
                if not f.result:
                    continue
                if f.fails:
                    hit = next((a for a in accept if a.matches(w, engine, m)
                                or a.matches(cid.workload, engine, m)), None)
                    if hit:
                        f.result, f.reason = "accepted", hit.reason
                findings.append(f)
    return findings


# ---------------------------------------------------------------------------
# Timing
# ---------------------------------------------------------------------------

@dataclass
class Cell:
    workload: str
    clients: int
    engine: str
    metric: str
    result: str                # regressed, improved, inconclusive, same,
                               # no interval; then confirmed / transient
    ratio: Optional[float] = None
    low: Optional[float] = None
    high: Optional[float] = None
    over_heap: dict = field(default_factory=dict)
    attempts: list = field(default_factory=list)   # [result, ...] per attempt
    problems: list = field(default_factory=list)
    reason: str = ""


def _rows(run: Path) -> tuple[list[bcompare.Row], list[str]]:
    """The comparison's rows, and the run's evidence problems."""
    sets = [table.load_set(p) for p in sorted(run.iterdir()) if (p / "run.json").exists()]
    problems = [f"{s.path.name}: {p}" for s in sets for p in s.problems]
    rows = bcompare.compare(sets)
    if not rows:
        problems.append("no comparison of two builds in the run")
    return rows, problems


def gate_timing(run: Path, retries: list[Path], t: dict,
                accept: list[Acceptance]) -> list[Cell]:
    retry_rows = [{(r.key, r.engine): r for r in _rows(p)[0]} for p in retries]
    rows, problems = _rows(run)
    # (evidence that fails is an invariant: it fails the gate, timing
    # reported or not)
    cells = [Cell("-", 0, "-", "evidence", "invalid", problems=[p]) for p in problems]
    for row in rows:
        w = f"{row.workload}/{row.variant}" if row.variant else row.workload
        if row.problems or all(r.estimate is None for r in row.ratios.values()):
            cells.append(Cell(w, row.clients, row.engine, "-", "invalid",
                              problems=row.problems or ["no repetition of the cell compared"]))
            continue
        for m, _, higher in bcompare.METRICS:
            ratio = row.ratios.get(m)
            if ratio is None or ratio.estimate is None:
                continue
            result = bcompare.regression(ratio, higher, t["timing_threshold"])
            cell = Cell(w, row.clients, row.engine, m, result, ratio.estimate, ratio.low,
                        ratio.high, dict(row.over_heap), [result], list(row.problems))
            if result == "regressed":
                for again in retry_rows[:t["attempts"] - 1]:
                    r = again.get((row.key, row.engine))
                    later = r.ratios.get(m) if r else None
                    cell.attempts.append(bcompare.regression(later, higher, t["timing_threshold"])
                                         if later and later.estimate is not None else "missing")
                cell.result = "confirmed" if all(a == "regressed" for a in cell.attempts) \
                    and len(cell.attempts) == t["attempts"] else "transient"
                if cell.result == "confirmed":
                    hit = next((a for a in accept if a.matches(w, row.engine, m)
                                or a.matches(row.workload, row.engine, m)), None)
                    if hit:
                        cell.result, cell.reason = "accepted", hit.reason
            cells.append(cell)
    return cells


def regressed_workloads(cells: list[Cell]) -> list[str]:
    """The workloads to run again: those with a cell that regressed."""
    return sorted({c.workload.split("/")[0] for c in cells if c.result in ("transient", "confirmed")})


# ---------------------------------------------------------------------------
# Command
# ---------------------------------------------------------------------------

def _acceptances(args) -> list[Acceptance]:
    """The entries the pull request adds: --accept's that --accept-base
    hasn't.  Both files must exist (the base's empty if it has none)."""
    if not args.accept:
        return []
    if not args.accept_base:
        raise SystemExit("--accept needs --accept-base: only entries the pull request adds count")
    for path in (args.accept, args.accept_base):
        if not path.exists():
            raise SystemExit(f"{path}: no such file")
    return added(load_accept(args.accept), load_accept(args.accept_base))


def _finite(value):
    """value with every non-finite float made None, for strict JSON."""
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, dict):
        return {k: _finite(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_finite(v) for v in value]
    return value


def run(args) -> int:
    t = thresholds(args.thresholds)
    accept = _acceptances(args)
    if args.layer == "counters":
        if len(args.runs) != 4:
            raise SystemExit("sgate counters BASE BASE2 CANDIDATE CANDIDATE2")
        findings = gate_counters(*args.runs, accept, t)
        failed = [f for f in findings if f.fails]
        out = {"layer": "counters", "thresholds": t, "passed": not failed,
               "findings": [asdict(f) for f in findings],
               "accepted": [asdict(a) for a in accept]}
        for f in failed:
            print(f"FAIL {f.workload} {f.engine} {f.counter}: {f.result} "
                  f"{f.base} -> {f.candidate} {f.detail}", file=sys.stderr)
    else:
        if not args.runs:
            raise SystemExit("sgate timing RUN [RETRY ...]")
        cells = gate_timing(args.runs[0], args.runs[1:], t, accept)
        failed = [c for c in cells if c.result == "invalid" or
                  (c.result == "confirmed" and not args.report_only)]
        out = {"layer": "timing", "thresholds": t, "report_only": args.report_only,
               "passed": not failed, "cells": [asdict(c) for c in cells],
               "rerun": regressed_workloads(cells)}
        for c in cells:
            if c.result in ("confirmed", "transient"):
                print(f"{c.result.upper()} {c.workload} c{c.clients} {c.engine} {c.metric}: "
                      f"{c.ratio:.3f} [{c.low:.3f}, {c.high:.3f}]", file=sys.stderr)
            elif c.result == "invalid":
                print(f"INVALID {c.workload} {c.engine}: {'; '.join(c.problems)}", file=sys.stderr)
    text = json.dumps(_finite(out), indent=2, allow_nan=False, default=str)
    if args.json:
        args.json.write_text(text + "\n")
    print(f"S suite {args.layer}: {'passed' if out['passed'] else 'FAILED'}")
    return 0 if out["passed"] else 1
