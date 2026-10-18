"""Two builds of the server compared, engine by engine: each candidate cell
against the same engine's base cell, as a paired ratio with its bootstrap
interval, and prolly's ratio to heap in the base and in the candidate.

The result sets are a run's with --pg-bin base=DIR,candidate=DIR: one per
(build, engine), the cells of each group run in random order with the same
seed per repetition.  `prolly_bench table` shows this for such sets.
"""

from __future__ import annotations

import math
import statistics
from dataclasses import dataclass, field
from typing import Optional

from pbench.core.stats import paired_ratio_interval
from pbench.core.validation import cell_problems, compatible

# (metric, heading, higher is better)
METRICS = [("throughput", "TPS ratio", True),
           ("latency_p50_ms", "p50 ratio", False),
           ("latency_p99_ms", "p99 ratio", False)]


def metric(r: dict, name: str) -> Optional[float]:
    m = r["metrics"]
    if name == "throughput":
        return m.get("tps") if m.get("tps") is not None else m.get("rows_per_second")
    return m.get(name)


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


def compare(sets) -> list[Row]:
    """Rows for result sets (table.ResultSet) of two builds: the base is the
    build named "base", else the first build seen."""
    builds = list(dict.fromkeys(s.build for s in sets if s.build))
    if len(builds) < 2:
        return []
    base_build = "base" if "base" in builds else builds[0]
    index = {(s.build, s.table_am): s for s in sets}
    rows = []
    for cand_build in [b for b in builds if b != base_build]:
        for engine in sorted({s.table_am for s in sets}, key=lambda e: e != "heap"):
            b, c = index.get((base_build, engine)), index.get((cand_build, engine))
            if not b or not c:
                continue
            refused = compatible(b.path, c.path, "build")
            base_cells, cand_cells = _by_rep(b.results), _by_rep(c.results)
            for key in sorted(set(base_cells) | set(cand_cells),
                              key=lambda k: (int(k[0][1:]), k[1], k[2], k[3])):
                row = Row(*key, engine)
                row.problems += refused
                for name, _, _ in METRICS:
                    ratio, problems = paired(base_cells.get(key, {}), cand_cells.get(key, {}), name)
                    row.ratios[name] = ratio
                    row.problems += [p for p in problems if p not in row.problems]
                if engine != "heap":
                    for build in (base_build, cand_build):
                        heap = index.get((build, "heap"))
                        if heap:
                            r, _ = paired(_by_rep(heap.results).get(key, {}),
                                          _by_rep(index[(build, engine)].results).get(key, {}),
                                          "throughput")
                            row.over_heap[build] = r.estimate
                rows.append(row)
    return rows


def regression(ratio: Ratio, higher_is_better: bool, threshold: float) -> str:
    """"regressed", "improved", "inconclusive" (the interval straddles the
    threshold), "no interval" or "same", at threshold (0.05: 5%)."""
    if ratio.low is None:
        return "no interval"
    worse_low, worse_high = (1 - threshold, 1 + threshold)
    if higher_is_better:
        if ratio.high < worse_low:
            return "regressed"
        if ratio.low > worse_high:
            return "improved"
        return "inconclusive" if ratio.low < worse_low else "same"
    if ratio.low > worse_high:
        return "regressed"
    if ratio.high < worse_low:
        return "improved"
    return "inconclusive" if ratio.high > worse_high else "same"


def to_markdown(rows: list[Row]) -> str:
    if not rows:
        return ""
    out = ["## Candidate over base (median ratio, 95% CI)\n",
           "| workload | variant | clients | engine | " + " | ".join(h for _, h, _ in METRICS)
           + " | prolly/heap base → candidate |",
           "|---|---|---:|---|" + "---:|" * len(METRICS) + "---|"]
    for row in rows:
        heap = " → ".join("-" if v is None else f"{v:.2f}" for v in row.over_heap.values()) \
            if row.over_heap else ""
        out.append(f"| {row.workload} | {row.variant or '-'} | {row.clients} | {row.engine} | "
                   + " | ".join(row.ratios[m].text() for m, _, _ in METRICS) + f" | {heap} |")
    problems = [f"- {r.workload} {r.engine}: {p}" for r in rows for p in r.problems]
    if problems:
        out += ["", "Problems:", *dict.fromkeys(problems)]
    return "\n".join(out) + "\n"
