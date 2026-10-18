"""Calibration of the S suite's gate: A/A runs (one build against itself)
measured for the noise the gate must live with.

  prolly_bench scalibrate --counters RUN RUN ... --timing RUN ... \\
      [--thresholds configs/s_gate.toml] [--out calibration.md] [--json ...]

--counters: counter runs of one build (configs/s_counters.toml), each a
directory of result sets; --timing: timing runs of a build against itself
(configs/s_pr.toml with --pg-bin base=DIR,candidate=DIR).  Reports:

- each counter's stability: in how many runs it took its most common value,
  and its spread (max - min, over the median);
- the instruction counts' noise (the largest relative spread);
- the gate's false failures on counters: of every four runs taken as the
  base, the base again, the candidate and the candidate again (or a seeded
  sample of them), those the gate would fail (a counter unstable only in
  the candidate, or regressed by the rule);
- each timing cell's interval width, and how often an A/A cell regressed
  at 5% and at 10% (a retried cell fails only if it regresses three times,
  about that rate cubed if attempts are independent);
- the values the noise suggests (suggest()).

The target is a false failure on fewer than 1% of pull requests.
"""

from __future__ import annotations

import itertools
import random
import json
import statistics
from collections import Counter
from pathlib import Path

from pbench import bcompare, scounters, table


def _counter_runs(paths: list[Path]) -> list[dict]:
    runs = []
    for p in paths:
        sets = [p] if (p / "run.json").exists() else sorted(
            q for q in p.iterdir() if (q / "run.json").exists())
        cells = {}
        for s in sets:
            rs = table.load_set(s)
            cells.update(scounters.counters(rs.results, rs.table_am))
        runs.append(cells)
    return runs


def counter_noise(runs: list[dict], t: dict) -> dict:
    stats = []
    for cid in sorted(set().union(*runs), key=str):
        names = set().union(*(r[cid].counters for r in runs if cid in r))
        for m in sorted(names):
            values = [r[cid].counters[m] for r in runs if cid in r and m in r[cid].counters]
            if len(values) < 2:
                continue
            mode, count = Counter(values).most_common(1)[0]
            median = statistics.median(values)
            spread = (max(values) - min(values)) / median if median else (
                0.0 if max(values) == min(values) else float("inf"))
            stats.append({"cell": str(cid), "counter": m, "kind": scounters.kind(m),
                          "runs": len(values), "at_mode": count, "stable": count == len(values),
                          "spread": spread, "range": max(values) - min(values)})
    # The gate on four runs as base, base again, candidate, candidate again:
    # every such choice of distinct runs, or a seeded sample of them.
    n = len(runs)
    if n * (n - 1) * (n - 2) * (n - 3) <= SAMPLE:
        trials = list(itertools.permutations(range(n), 4))
    else:
        rng = random.Random(0)
        trials = [tuple(rng.sample(range(n), 4)) for _ in range(SAMPLE)]
    failing_trials = 0
    failing = Counter()
    ranges = {"events": 0.0, "bytes": 0.0}
    for quad in trials:
        hit = False
        for cid, m, base, cand in _gate(t, *(runs[i] for i in quad)):
            failing[(str(cid), m)] += 1
            k = scounters.kind(m)
            if k in ranges:
                ranges[k] = max(ranges[k], abs(cand - base))
            hit = True
        failing_trials += hit
    instructions = [s["spread"] for s in stats if s["kind"] == "instructions"]
    exact = [s for s in stats if s["kind"] != "instructions"]
    return {
        "runs": len(runs), "counters": len(stats),
        "stable": sum(s["stable"] for s in exact), "exact_counters": len(exact),
        "unstable": [s for s in exact if not s["stable"]],
        "instruction_spread_max": max(instructions) if instructions else None,
        "trials": len(trials), "failing_trials": failing_trials,
        "false_failure_rate": failing_trials / len(trials) if trials else None,
        "failing_counters": [{"cell": c, "counter": m, "trials": k}
                             for (c, m), k in failing.most_common()],
        # the largest difference that failed the gate, by kind of floor
        "largest_failing_difference": ranges,
    }


SAMPLE = 2000


def _gate(t: dict, base: dict, base2: dict, cand: dict, cand2: dict):
    """The counters the gate would fail on (no build changed anything):
    (cell, counter, base, candidate) for each unstable only in the
    candidate, or regressed."""
    in_base = scounters.instability(base, base2, t)
    in_cand = scounters.instability(cand, cand2, t)
    for cid in set(base) & set(base2) & set(cand) & set(cand2):
        b, c = base[cid].counters, cand[cid].counters
        for m in set(b) & set(base2[cid].counters) & set(c) & set(cand2[cid].counters):
            if in_base.get((cid, m)) == "unstable":
                continue                      # unstable in the base: reported
            if in_cand.get((cid, m)) == "unstable" or \
                    scounters.change(m, b[m], c[m], t) == "regression":
                yield cid, m, b[m], c[m]


def timing_noise(paths: list[Path]) -> dict:
    cells = []
    runs_failing = {0.05: 0, 0.10: 0}
    for p in paths:
        rows = bcompare.compare([table.load_set(q) for q in sorted(p.iterdir())
                                 if (q / "run.json").exists()])
        hit = {0.05: False, 0.10: False}
        for row in rows:
            for m, _, higher in bcompare.METRICS:
                r = row.ratios.get(m)
                if not r or r.low is None:
                    continue
                verdict = {thr: bcompare.regression(r, higher, thr) for thr in hit}
                for thr, v in verdict.items():
                    hit[thr] |= v == "regressed"
                cells.append({"run": p.name, "workload": row.workload, "variant": row.variant,
                              "clients": row.clients, "engine": row.engine, "metric": m,
                              "ratio": r.estimate, "low": r.low, "high": r.high,
                              "width": r.high - r.low,
                              "regressed_5": verdict[0.05] == "regressed",
                              "regressed_10": verdict[0.10] == "regressed"})
        for thr in hit:
            runs_failing[thr] += hit[thr]
    by = {}
    for c in cells:
        by.setdefault((c["workload"], c["variant"], c["clients"], c["engine"], c["metric"]), []).append(c)
    summary = [{"workload": k[0], "variant": k[1], "clients": k[2], "engine": k[3], "metric": k[4],
                "runs": len(v), "median_width": statistics.median(c["width"] for c in v),
                "max_width": max(c["width"] for c in v),
                "regressed_5": sum(c["regressed_5"] for c in v),
                "regressed_10": sum(c["regressed_10"] for c in v)} for k, v in by.items()]
    n = len(paths)
    return {"runs": n, "cells": summary,
            "runs_with_a_regressed_cell": {f"{int(thr * 100)}%": runs_failing[thr] for thr in runs_failing},
            "cell_regression_rate": {
                "5%": sum(c["regressed_5"] for c in cells) / len(cells) if cells else None,
                "10%": sum(c["regressed_10"] for c in cells) / len(cells) if cells else None}}


def suggest(counters: dict | None, t: dict) -> dict:
    """The settings the noise suggests: an instruction tolerance of twice
    the largest spread, and, if the gate failed A/A runs on 1% or more,
    floors twice the largest difference it failed on.  (An unstable
    candidate counter fails whatever its floor: a run shape problem, listed
    in the report, not one a setting fixes.)"""
    out = dict(t)
    if counters and counters["instruction_spread_max"] is not None:
        out["instructions_tolerance"] = round(max(0.001, 2 * counters["instruction_spread_max"]), 4)
    if counters and (counters["false_failure_rate"] or 0) >= 0.01:
        ranges = counters["largest_failing_difference"]
        out["floor_events"] = max(t["floor_events"], ranges["events"] * 2)
        out["floor_bytes"] = max(t["floor_bytes"], ranges["bytes"] * 2)
    return out


def to_markdown(counters: dict | None, timing: dict | None, suggested: dict) -> str:
    out = ["# S suite gate calibration (A/A)", ""]
    if counters:
        rate = counters["false_failure_rate"]
        spread = counters["instruction_spread_max"]
        out += [f"## Counters: {counters['runs']} runs of one build", "",
                f"- {counters['stable']} of {counters['exact_counters']} exact counters took one "
                "value in every run",
                "- instruction counts' largest spread: "
                + ("not counted" if spread is None else f"{spread:.4%}"),
                f"- false failures: {counters['failing_trials']} of {counters['trials']} gates on "
                f"four runs ({'-' if rate is None else f'{rate:.1%}'})", ""]
        if counters["failing_counters"]:
            out += ["Failing: " + ", ".join(f"{f['cell']} {f['counter']} ({f['trials']})"
                                            for f in counters["failing_counters"][:20]), ""]
        if counters["unstable"]:
            out += ["| cell | counter | runs at its mode | spread |", "|---|---|---:|---:|"]
            out += [f"| {s['cell']} | {s['counter']} | {s['at_mode']}/{s['runs']} | {s['spread']:.3%} |"
                    for s in counters["unstable"]]
            out.append("")
    if timing:
        out += [f"## Timing: {timing['runs']} A/A runs", "",
                f"- runs with some cell regressed: {timing['runs_with_a_regressed_cell']}",
                f"- cells regressed: {timing['cell_regression_rate']}", "",
                "| workload | clients | engine | metric | median CI width | max | regressed at 5% | at 10% |",
                "|---|---:|---|---|---:|---:|---:|---:|"]
        out += [f"| {c['workload']}{'/' + c['variant'] if c['variant'] else ''} | {c['clients']} | "
                f"{c['engine']} | {c['metric']} | {c['median_width']:.3f} | {c['max_width']:.3f} | "
                f"{c['regressed_5']}/{c['runs']} | {c['regressed_10']}/{c['runs']} |"
                for c in timing["cells"]]
        out.append("")
    out += ["## Suggested gate settings", "", "```toml", "[gate]"]
    out += [f"{k} = {v}" for k, v in suggested.items()]
    out += ["```", ""]
    return "\n".join(out)


def run(args) -> int:
    t = scounters.thresholds(args.thresholds)
    counters = counter_noise(_counter_runs(args.counters), t) if args.counters else None
    timing = timing_noise(args.timing) if args.timing else None
    suggested = suggest(counters, t)
    md = to_markdown(counters, timing, suggested)
    if args.out:
        args.out.write_text(md)
    if args.json:
        args.json.write_text(json.dumps({"counters": counters, "timing": timing,
                                         "suggested": suggested}, indent=2) + "\n")
    print(md)
    return 0
