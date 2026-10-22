"""Two builds of the server compared, engine by engine: each candidate cell
against the same engine's base cell, as a paired ratio with its bootstrap
interval, and prolly's ratio to heap in the base and in the candidate.

The result sets are a run's with --pg-bin base=DIR,candidate=DIR: one per
(build, engine), the cells of each group run in random order with the same
seed per repetition.  The base is the build --pg-bin named first (run.json
records the order).  `prolly_bench table` shows this for such sets.
"""

from __future__ import annotations

import math
import statistics
from dataclasses import dataclass, field
from typing import Optional

from pbench.core.stats import paired_ratio_interval
from pbench.core.validation import cell_problems, compatible
from pbench.table import _metric as metric

# (metric, heading, higher is better)
METRICS = [("throughput", "TPS ratio", True),
           ("latency_p50_ms", "p50 ratio", False),
           ("latency_p99_ms", "p99 ratio", False)]

THRESHOLD = 0.05       # minimum practical effect for the verdict shown by `table`


@dataclass
class Ratio:
    estimate: Optional[float]          # median of the per-repetition ratios
    low: Optional[float] = None        # its 95% interval, given 5 pairs or more
    high: Optional[float] = None
    pairs: int = 0

    def text(self) -> str:
        if self.estimate is None:
            return "-"
        if self.low is None:
            return f"{self.estimate:.2f} (n={self.pairs})"
        return f"{self.estimate:.2f} [{self.low:.2f}, {self.high:.2f}]"


@dataclass
class Row:
    workload: str
    variant: str
    size: str
    clients: int
    engine: str
    ratios: dict[str, Ratio] = field(default_factory=dict)
    # prolly over heap, per build (prolly rows only)
    over_heap: dict[str, Optional[float]] = field(default_factory=dict)
    problems: list[str] = field(default_factory=list)

    @property
    def key(self) -> tuple:
        return (self.workload, self.variant, self.size, self.clients)

    def name(self) -> str:
        v = f"/{self.variant}" if self.variant else ""
        return f"{self.workload}{v} {self.size} c{self.clients} {self.engine}"


def _by_rep(results: list[dict]) -> dict[tuple, dict[int, dict]]:
    out: dict[tuple, dict[int, dict]] = {}
    for r in results:
        k = r["key"]
        out.setdefault((k["workload"], k["variant"], k["size"], k["clients"]), {})[k["rep"]] = r
    return out


def paired(base: dict[int, dict], cand: dict[int, dict], name: str) -> tuple[Ratio, list[str]]:
    """cand over base, repetition by repetition, for metric name."""
    problems = []
    pairs = []
    for rep in sorted(set(base) | set(cand)):
        b, c = base.get(rep), cand.get(rep)
        if b is None or c is None:
            problems.append(f"repetition {rep} missing from a build")
            continue
        if cell_problems(b) or cell_problems(c):
            problems.append(f"repetition {rep} invalid")
            continue
        if b.get("measurement", {}).get("seed") != c.get("measurement", {}).get("seed"):
            problems.append(f"repetition {rep} ran with different seeds")
            continue
        x, y = metric(c, name), metric(b, name)
        if x is not None and y is not None and x > 0 and y > 0 and math.isfinite(x) and math.isfinite(y):
            pairs.append((x, y))
    if not pairs:
        return Ratio(None), problems
    interval = paired_ratio_interval(pairs)
    if interval:
        return Ratio(*interval, pairs=len(pairs)), problems
    return Ratio(statistics.median(a / b for a, b in pairs), pairs=len(pairs)), problems


def base_build(sets) -> Optional[str]:
    """The base: the first build the run's --pg-bin named (or, for result
    sets from before run.json recorded the order, the one named "base")."""
    for s in sets:
        if s.builds:
            return s.builds[0]
    return "base" if any(s.build == "base" for s in sets) else None


def compare(sets) -> list[Row]:
    """Rows for result sets (table.ResultSet) of two or more builds."""
    builds = list(dict.fromkeys(s.build for s in sets if s.build))
    base = base_build(sets)
    if len(builds) < 2 or base not in builds:
        return []
    index = {(s.build, s.table_am): s for s in sets}
    reps = {k: _by_rep(s.results) for k, s in index.items()}
    rows = []
    for cand in [b for b in builds if b != base]:
        for engine in sorted({s.table_am for s in sets}, key=lambda e: e != "heap"):
            b, c = index.get((base, engine)), index.get((cand, engine))
            if not b or not c:
                continue
            refused = compatible(b.path, c.path, "build")
            base_cells, cand_cells = reps[(base, engine)], reps[(cand, engine)]
            for key in sorted(set(base_cells) | set(cand_cells),
                              key=lambda k: (int(k[0][1:]), k[1], k[2], k[3])):
                row = Row(*key, engine)
                row.problems += refused
                for name, _, _ in METRICS:
                    ratio, problems = paired(base_cells.get(key, {}), cand_cells.get(key, {}), name)
                    row.ratios[name] = ratio
                    row.problems += [p for p in problems if p not in row.problems]
                if engine != "heap":
                    for build in (base, cand):
                        if (build, "heap") in reps:
                            r, _ = paired(reps[(build, "heap")].get(key, {}),
                                          reps[(build, engine)].get(key, {}), "throughput")
                            row.over_heap[build] = r.estimate
                rows.append(row)
    return rows


def regression(ratio: Ratio, higher_is_better: bool, threshold: float) -> str:
    """At threshold (0.05: 5%): "regressed" or "improved" when the whole
    interval lies beyond it on that side, "inconclusive" when the interval
    straddles either side's threshold, "same" when it lies within both,
    and "no interval" without one."""
    if ratio.low is None:
        return "no interval"
    below, above = 1 - threshold, 1 + threshold
    worse_low = ratio.high < below if higher_is_better else ratio.low > above
    better = ratio.low > above if higher_is_better else ratio.high < below
    if worse_low:
        return "regressed"
    if better:
        return "improved"
    if ratio.low < below or ratio.high > above:
        return "inconclusive"
    return "same"


def verdict(row: Row, threshold: float = THRESHOLD) -> str:
    """The row's verdicts, the worst first ("regressed" if any metric did)."""
    order = ["regressed", "inconclusive", "improved", "same", "no interval"]
    found = {regression(row.ratios[m], higher, threshold)
             for m, _, higher in METRICS
             if m in row.ratios and row.ratios[m].estimate is not None}
    return ", ".join(v for v in order if v in found) or "-"


def to_markdown(rows: list[Row]) -> str:
    if not rows:
        return ""
    out = [f"## Candidate over base (median ratio, 95% CI; verdict at {THRESHOLD:.0%})\n",
           "| workload | variant | size | clients | engine | "
           + " | ".join(h for _, h, _ in METRICS) + " | prolly/heap base → candidate | verdict |",
           "|---|---|---|---:|---|" + "---:|" * len(METRICS) + "---|---|"]
    for row in rows:
        heap = " → ".join("-" if v is None else f"{v:.2f}" for v in row.over_heap.values()) \
            if row.over_heap else ""
        out.append(f"| {row.workload} | {row.variant or '-'} | {row.size} | {row.clients} | "
                   f"{row.engine} | " + " | ".join(row.ratios[m].text() for m, _, _ in METRICS)
                   + f" | {heap} | {verdict(row)} |")
    problems = [f"- {r.name()}: {p}" for r in rows for p in r.problems]
    if problems:
        out += ["", "Problems:", *dict.fromkeys(problems)]
    return "\n".join(out) + "\n"
