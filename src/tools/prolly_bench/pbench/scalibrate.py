"""Calibration of the S suite's gate: A/A runs (one build against itself)
measured for the noise the gate must live with.

  prolly_bench scalibrate --counters RUN RUN ... --timing RUN ... \\
      [--thresholds configs/s_gate.toml] [--out calibration.md] [--json ...]

--counters: counter runs of one build (configs/s_counters.toml), each a
directory of result sets, named counters-<runner>-<n> when several runners
made them; --timing: timing runs of a build against itself (configs/s_pr.toml
with --pg-bin base=DIR,candidate=DIR).  A run whose evidence is incomplete
or invalid is left out, and listed.  Reports:

- each counter's stability: in how many runs it took its most common value
  (a run without it counting as another value), and its spread (max - min,
  over the median);
- the instruction counts' noise (the largest relative spread, within a
  runner: a pull request's base and candidate share one);
- the gate's false failures on counters: the gate applied to four distinct
  runs of one runner as the base, the base again, the candidate and the
  candidate again (every such choice, or a seeded sample), and the same
  across runners, for comparison;
- each timing cell's interval width, and how often an A/A cell regressed at
  the gate's threshold and at 5% and 10% (a retried cell fails only if it
  regresses in every attempt: about that rate to the power of attempts if
  attempts are independent);
- the values the noise suggests (suggest()).

The target is a false failure on fewer than 1% of pull requests.
"""

from __future__ import annotations

import itertools
import json
import random
import re
import statistics
from collections import Counter
from pathlib import Path

from pbench import bcompare, scounters, table

SAMPLE = 2000
_RUNNER = re.compile(r"counters-(\d+)-\d+$")


def _counter_runs(paths: list[Path]) -> tuple[list[tuple[str, dict]], list[str]]:
    """[(runner, cells)] of the counter runs with complete, valid evidence,
    and why the others were left out."""
    runs, left_out = [], []
    for p in paths:
        try:
            sets = [p] if (p / "run.json").exists() else sorted(
                q for q in p.iterdir() if (q / "run.json").exists())
        except OSError as e:
            left_out.append(f"{p}: {e}")
            continue
        loaded = [table.load_set(s) for s in sets]
        problems = [f"{s.path.name}: {problem}" for s in loaded for problem in s.problems]
        if not loaded or problems:
            left_out.append(f"{p}: " + ("; ".join(problems[:3]) or "no result sets"))
            continue
        cells = {}
        for rs in loaded:
            cells.update(scounters.counters(rs.results, rs.table_am))
        m = _RUNNER.search(p.name)
        runs.append((m.group(1) if m else "", cells))
    return runs, left_out


def _trials(indices: list[int], rng: random.Random, limit: int) -> list[tuple]:
    n = len(indices)
    if n < 4:
        return []
    if n * (n - 1) * (n - 2) * (n - 3) <= limit:
        return list(itertools.permutations(indices, 4))
    return [tuple(rng.sample(indices, 4)) for _ in range(limit)]


def counter_noise(named_runs: list[tuple[str, dict]], t: dict) -> dict:
    runs = [cells for _, cells in named_runs]
    stats = []
    for cid in sorted(set().union(*runs) if runs else (), key=str):
        having = [r[cid] for r in runs if cid in r]
        names = set().union(*(c.counters for c in having))
        for m in sorted(names):
            values = [c.counters.get(m) for c in having]
            present = [v for v in values if v is not None]
            mode, count = Counter(present).most_common(1)[0]
            median = statistics.median(present)
            spread = (max(present) - min(present)) / median if median else (
                0.0 if max(present) == min(present) else float("inf"))
            stats.append({"cell": str(cid), "counter": m, "kind": scounters.kind(m),
                          "runs": len(values), "at_mode": count, "stable": count == len(values),
                          "spread": spread, "range": max(present) - min(present)})

    # The gate on four runs: within each runner (as a pull request's runs
    # are), and across runners for comparison.
    rng = random.Random(0)
    by_runner: dict[str, list[int]] = {}
    for i, (runner, _) in enumerate(named_runs):
        by_runner.setdefault(runner, []).append(i)
    within = [q for idx in by_runner.values() for q in _trials(idx, rng, SAMPLE)]
    across = _trials(list(range(len(runs))), rng, SAMPLE) if len(by_runner) > 1 else []
    cache: dict[tuple[int, int], dict] = {}

    def instability(i: int, j: int) -> dict:
        if (i, j) not in cache:
            cache[(i, j)] = scounters.instability(runs[i], runs[j], t)
        return cache[(i, j)]

    def simulate(trials):
        failing_trials, failing = 0, Counter()
        ranges = {"events": 0.0, "bytes": 0.0}
        for b, b2, c, c2 in trials:
            hit = False
            for cid, m, why, base, cand in _gate(t, runs[b], runs[c],
                                                 instability(b, b2), instability(c, c2)):
                failing[(str(cid), m, why)] += 1
                k = scounters.kind(m)
                if why == "regression" and k in ranges:
                    ranges[k] = max(ranges[k], cand - base)
                hit = True
            failing_trials += hit
        return failing_trials, failing, ranges

    failing_trials, failing, ranges = simulate(within)
    across_failing, _, _ = simulate(across)
    # Instruction spread within a runner: what a pull request's runs see.
    instructions = []
    for idx in by_runner.values():
        for i, j in itertools.combinations(idx, 2):
            for cid in set(runs[i]) & set(runs[j]):
                a, b = runs[i][cid].counters, runs[j][cid].counters
                for m in set(a) & set(b):
                    if scounters.kind(m) == "instructions" and max(a[m], b[m]) > 0:
                        instructions.append(abs(a[m] - b[m]) / max(a[m], b[m]))
    exact = [s for s in stats if s["kind"] != "instructions"]
    return {
        "runs": len(runs), "runners": len(by_runner), "counters": len(stats),
        "stable": sum(s["stable"] for s in exact), "exact_counters": len(exact),
        "unstable": [s for s in exact if not s["stable"]],
        "instruction_spread_max": max(instructions) if instructions else None,
        "trials": len(within), "failing_trials": failing_trials,
        "false_failure_rate": failing_trials / len(within) if within else None,
        "failing_counters": [{"cell": c, "counter": m, "why": why, "trials": k}
                             for (c, m, why), k in failing.most_common()],
        "across_runners": {"trials": len(across), "failing_trials": across_failing,
                           "false_failure_rate": across_failing / len(across) if across else None},
        # the largest regression the gate failed on, by kind of floor
        "largest_failing_difference": ranges,
    }


def _gate(t: dict, base: dict, cand: dict, in_base: dict, in_cand: dict):
    """What the gate would fail on when no build changed anything, as the
    real gate decides it: (cell, counter, why, base, candidate), why being
    "unstable" (only in the candidate, a missing counter included) or
    "regression"."""
    for cid in sorted(set(base) | set(cand), key=str):
        b = base[cid].counters if cid in base else {}
        c = cand[cid].counters if cid in cand else {}
        for m in sorted(set(b) | set(c)):
            if in_base.get((cid, m)) == "unstable":
                continue                      # unstable in the base: reported
            if in_cand.get((cid, m)) == "unstable":
                yield cid, m, "unstable", b.get(m), c.get(m)
            elif m in b and m in c and scounters.change(m, b[m], c[m], t) == "regression":
                yield cid, m, "regression", b[m], c[m]


def timing_noise(paths: list[Path], t: dict) -> dict:
    thresholds = sorted({t["timing_threshold"], 0.05, 0.10})
    cells, invalid, left_out = [], [], []
    runs_failing = {thr: 0 for thr in thresholds}
    used = 0
    for p in paths:
        try:
            sets = [table.load_set(q) for q in sorted(p.iterdir()) if (q / "run.json").exists()]
        except OSError as e:
            left_out.append(f"{p}: {e}")
            continue
        problems = [f"{s.path.name}: {x}" for s in sets for x in s.problems]
        rows = bcompare.compare(sets)
        if problems or not rows:
            left_out.append(f"{p}: " + ("; ".join(problems[:3]) or "no comparison of builds"))
            continue
        used += 1
        hit = {thr: False for thr in thresholds}
        for row in rows:
            if row.problems:
                invalid.append(f"{p.name} {row.name()}: " + "; ".join(row.problems))
                continue
            for m, _, higher in bcompare.METRICS:
                r = row.ratios.get(m)
                if not r or r.low is None:
                    continue
                verdict = {thr: bcompare.regression(r, higher, thr) == "regressed"
                           for thr in thresholds}
                for thr, v in verdict.items():
                    hit[thr] |= v
                cells.append({"run": p.name, "workload": row.workload, "variant": row.variant,
                              "size": row.size, "clients": row.clients, "engine": row.engine,
                              "metric": m, "ratio": r.estimate, "low": r.low, "high": r.high,
                              "width": r.high - r.low, "regressed": verdict})
        for thr in thresholds:
            runs_failing[thr] += hit[thr]
    by = {}
    for c in cells:
        by.setdefault((c["workload"], c["variant"], c["size"], c["clients"], c["engine"],
                       c["metric"]), []).append(c)
    summary = [{"workload": k[0], "variant": k[1], "size": k[2], "clients": k[3],
                "engine": k[4], "metric": k[5], "runs": len(v),
                "median_width": statistics.median(c["width"] for c in v),
                "max_width": max(c["width"] for c in v),
                "regressed": {f"{thr:.0%}": sum(c["regressed"][thr] for c in v)
                              for thr in thresholds}} for k, v in by.items()]
    rate = {f"{thr:.0%}": sum(c["regressed"][thr] for c in cells) / len(cells) if cells else None
            for thr in thresholds}
    gate = f"{t['timing_threshold']:.0%}"
    return {"runs": used, "left_out": left_out, "invalid_rows": invalid, "cells": summary,
            "runs_with_a_regressed_cell": {f"{thr:.0%}": runs_failing[thr] for thr in thresholds},
            "cell_regression_rate": rate,
            # a cell confirmed only if it regresses in every attempt
            "confirmed_rate_estimate": (rate[gate] ** t["attempts"]
                                        if rate[gate] is not None else None)}


def suggest(counters: dict | None, t: dict) -> dict:
    """The settings the noise suggests: an instruction tolerance of twice
    the largest spread within a runner, and, if the gate failed A/A runs
    on 1% or more, floors twice the largest regression it failed on.  (A
    counter unstable in the candidate fails whatever its floor: a run
    shape problem, listed in the report, not one a setting fixes.)"""
    out = dict(t)
    if counters and counters["instruction_spread_max"] is not None:
        out["instructions_tolerance"] = round(max(0.001, 2 * counters["instruction_spread_max"]), 4)
    if counters and (counters["false_failure_rate"] or 0) >= 0.01:
        ranges = counters["largest_failing_difference"]
        out["floor_events"] = max(t["floor_events"], ranges["events"] * 2)
        out["floor_bytes"] = max(t["floor_bytes"], ranges["bytes"] * 2)
    return out


def _rate(r) -> str:
    return "-" if r is None else f"{r:.1%}"


def to_markdown(counters: dict | None, timing: dict | None, suggested: dict,
                left_out: list[str] = ()) -> str:
    out = ["# S suite gate calibration (A/A)", ""]
    if left_out:
        out += ["Left out (incomplete or invalid evidence):", *[f"- {x}" for x in left_out], ""]
    if counters:
        spread = counters["instruction_spread_max"]
        across = counters["across_runners"]
        out += [f"## Counters: {counters['runs']} runs of one build on "
                f"{counters['runners']} runner(s)", "",
                f"- {counters['stable']} of {counters['exact_counters']} exact counters took one "
                "value in every run",
                "- instruction counts' largest spread within a runner: "
                + ("not counted" if spread is None else f"{spread:.4%}"),
                f"- false failures within a runner: {counters['failing_trials']} of "
                f"{counters['trials']} gates on four runs ({_rate(counters['false_failure_rate'])})",
                f"- across runners: {across['failing_trials']} of {across['trials']} "
                f"({_rate(across['false_failure_rate'])})", ""]
        if counters["failing_counters"]:
            out += ["Failing: " + ", ".join(f"{f['cell']} {f['counter']} {f['why']} ({f['trials']})"
                                            for f in counters["failing_counters"][:20]), ""]
        if counters["unstable"]:
            out += ["| cell | counter | runs at its mode | spread |", "|---|---|---:|---:|"]
            out += [f"| {s['cell']} | {s['counter']} | {s['at_mode']}/{s['runs']} | {s['spread']:.3%} |"
                    for s in counters["unstable"]]
            out.append("")
    if timing:
        thresholds = list(timing["runs_with_a_regressed_cell"])
        out += [f"## Timing: {timing['runs']} A/A runs", "",
                f"- runs with some cell regressed: {timing['runs_with_a_regressed_cell']}",
                f"- cells regressed: { {k: _rate(v) for k, v in timing['cell_regression_rate'].items()} }",
                f"- a cell confirmed (regressed in every attempt), if attempts are independent: "
                f"{_rate(timing['confirmed_rate_estimate'])}"]
        if timing["invalid_rows"]:
            out.append(f"- rows left out for their problems: {len(timing['invalid_rows'])}")
        out += ["", "| workload | size | clients | engine | metric | median CI width | max | "
                + " | ".join(f"regressed at {k}" for k in thresholds) + " |",
                "|---|---|---:|---|---|---:|---:|" + "---:|" * len(thresholds)]
        out += [f"| {c['workload']}{'/' + c['variant'] if c['variant'] else ''} | {c['size']} | "
                f"{c['clients']} | {c['engine']} | {c['metric']} | {c['median_width']:.3f} | "
                f"{c['max_width']:.3f} | "
                + " | ".join(f"{c['regressed'][k]}/{c['runs']}" for k in thresholds) + " |"
                for c in timing["cells"]]
        out.append("")
    out += ["## Suggested gate settings", "", "```toml", "[gate]"]
    out += [f"{k} = {v}" for k, v in suggested.items()]
    out += ["```", ""]
    return "\n".join(out)


def run(args) -> int:
    t = scounters.thresholds(args.thresholds)
    named, left_out = _counter_runs(args.counters) if args.counters else ([], [])
    counters = counter_noise(named, t) if named else None
    timing = timing_noise(args.timing, t) if args.timing else None
    if timing:
        left_out += timing["left_out"]
    suggested = suggest(counters, t)
    md = to_markdown(counters, timing, suggested, left_out)
    if args.out:
        args.out.write_text(md)
    if args.json:
        args.json.write_text(json.dumps({"counters": counters, "timing": timing,
                                         "suggested": suggested, "left_out": left_out},
                                        indent=2) + "\n")
    print(md)
    return 0 if counters or timing else 1
