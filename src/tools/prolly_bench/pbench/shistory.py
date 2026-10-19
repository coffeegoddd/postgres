"""A night's S results, summarized for the perf-history branch.

  prolly_bench shistory RUN --timing sgate.json [--instructions DIR] \\
      --date YYYY-MM-DD --out FILE.json

RUN is the nightly's timing run (its result sets, one per build and
engine); the summary keeps what a later night compares and charts with:
each cell's medians, the candidate-over-base ratios with their intervals
and verdicts (sgate timing's), the builds' commits, and each result set's
comparison identity.  The raw results stay in the run's artifact.
"""

from __future__ import annotations

import json
import statistics
from pathlib import Path

from pbench import bcompare, table

SCHEMA = 1


def medians(rs: table.ResultSet) -> list[dict]:
    by = {}
    for r in rs.results:
        if r.get("status") != "ok":
            continue
        k = r["key"]
        by.setdefault((k["workload"], k["variant"], k["size"], k["clients"]), []).append(r)
    out = []
    for (w, v, size, clients), rows in sorted(by.items()):
        cell = {"workload": w, "variant": v, "size": size, "clients": clients, "n": len(rows)}
        for m, _, _ in bcompare.METRICS:
            values = [x for x in (bcompare.metric(r, m) for r in rows) if x is not None]
            cell[m] = statistics.median(values) if values else None
        out.append(cell)
    return out


def summarize(run: Path, timing: dict, date: str, instructions: dict | None = None) -> dict:
    sets = [table.load_set(p) for p in sorted(run.iterdir()) if (p / "run.json").exists()]
    commits = {}
    out_sets = []
    for s in sets:
        run_json = json.loads((s.path / "run.json").read_text())
        # (a set whose run stopped early may have no environment: kept,
        # with its problems, and no commit)
        env_file = s.path / "environment.json"
        env = json.loads(env_file.read_text()) if env_file.exists() else {}
        provenance = env.get("postgres", {}).get("build_provenance") or {}
        commits[s.build] = provenance.get("commit")
        out_sets.append({"label": s.label, "build": s.build, "engine": s.table_am,
                         "identity": run_json.get("comparison_identity"),
                         "problems": s.problems, "cells": medians(s)})
    return {"schema": SCHEMA, "date": date, "commits": commits,
            "base": commits.get("base"), "candidate": commits.get("candidate"),
            "sets": out_sets, "comparison": timing.get("cells", []),
            "confirmed": [c for c in timing.get("cells", []) if c["result"] == "confirmed"],
            "instructions": instructions}


def instructions_of(run: Path) -> dict:
    """{cell: instructions per unit} of a cachegrind run's result sets."""
    out = {}
    for p in sorted(run.iterdir()):
        if not (p / "run.json").exists():
            continue
        rs = table.load_set(p)
        for r in rs.results:
            m = r.get("metrics", {})
            if m.get("cachegrind_instructions_per_operation") is not None:
                k = r["key"]
                out[f"{k['workload']}{'/' + k['variant'] if k['variant'] else ''} {rs.table_am}"] = \
                    m["cachegrind_instructions_per_operation"]
    return out


def run(args) -> int:
    timing = json.loads(args.timing.read_text()) if args.timing and args.timing.exists() else {}
    instructions = instructions_of(args.instructions) if args.instructions else None
    summary = summarize(args.run, timing, args.date, instructions)
    args.out.write_text(json.dumps(summary, indent=1, sort_keys=True) + "\n")
    print(f"{args.out}: {len(summary['sets'])} result sets, "
          f"{len(summary['confirmed'])} confirmed regressions")
    return 0
