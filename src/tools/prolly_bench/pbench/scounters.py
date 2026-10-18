"""Counters of the S suite's counter runs (configs/s_counters.toml): what
each workload did over its fixed work, read from a result set.

  prolly_bench scounters RUN RUN2      two runs of one build: which counters
                                       differ (they shouldn't), and why not

A counter is a total over the cell's measured work (the gate's floors are on
totals); per_unit is it over the work's units (transactions, rows,
queries).  Instruction counts aren't exact on any machine, so they compare
within a tolerance.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

# Totals that count work, exactly, in a counter run.
EXACT = ("wal_bytes", "wal_records", "wal_fpi", "wal_fpi_bytes",
         "io_hits", "io_reads", "io_writes", "io_extends",
         "relation_bytes", "chunk_store_bytes")
# Instruction counts: close, never exact.
INSTRUCTIONS = ("instructions_user", "instructions_kernel", "cachegrind_instructions")
# Each counter's unit and floor kind (events, bytes or instructions).
_BYTES = {"wal_bytes", "wal_fpi_bytes", "relation_bytes", "chunk_store_bytes",
          "engine.chunk_bytes_appended", "engine.bytes_hashed"}


def is_counter(metric: str) -> bool:
    if metric.startswith("engine."):
        return not metric.endswith("_per_operation")
    return metric in EXACT or metric in INSTRUCTIONS


def kind(metric: str) -> str:
    """"bytes", "instructions" or "events": which floor applies."""
    if metric in INSTRUCTIONS:
        return "instructions"
    return "bytes" if metric in _BYTES else "events"


@dataclass(frozen=True)
class CellId:
    workload: str
    variant: str
    size: str
    clients: int
    engine: str

    def __str__(self) -> str:
        v = f"/{self.variant}" if self.variant else ""
        return f"{self.workload}{v} {self.engine}"


@dataclass
class CellCounters:
    counters: dict[str, float]
    units: Optional[float]       # the work's denominator
    unit: str

    def per_unit(self, metric: str) -> Optional[float]:
        value = self.counters.get(metric)
        return None if value is None or not self.units else value / self.units


def counters(results: list[dict], engine: str) -> dict[CellId, CellCounters]:
    """The counters of a result set's successful cells (its results)."""
    out = {}
    for r in results:
        if r.get("status") != "ok":
            continue
        k = r["key"]
        cid = CellId(k["workload"], k.get("variant", ""), k["size"], k["clients"], engine)
        out[cid] = CellCounters(
            {m: float(v) for m, v in r.get("metrics", {}).items()
             if is_counter(m) and v is not None and math.isfinite(v)},
            r.get("denominator"), r.get("unit", "transaction"))
    return out


def differs(a: float, b: float, metric: str, tolerance: float) -> bool:
    if kind(metric) == "instructions":
        return abs(a - b) > tolerance * max(abs(a), abs(b), 1.0)
    return a != b


def unstable(one: dict[CellId, CellCounters], two: dict[CellId, CellCounters],
             tolerance: float = 0.0) -> list[tuple[CellId, str, Optional[float], Optional[float]]]:
    """Counters two runs of one build disagree on: (cell, counter, one, two);
    a counter one run has and the other hasn't counts."""
    out = []
    for cid in sorted(set(one) | set(two), key=str):
        a, b = one.get(cid), two.get(cid)
        names = set(a.counters if a else {}) | set(b.counters if b else {})
        for m in sorted(names):
            x = a.counters.get(m) if a else None
            y = b.counters.get(m) if b else None
            if x is None or y is None or differs(x, y, m, tolerance):
                out.append((cid, m, x, y))
        if a and b and a.units != b.units:
            out.append((cid, "units of work", a.units, b.units))
    return out


def load(path: Path) -> dict[CellId, CellCounters]:
    from pbench import table
    rs = table.load_set(path)
    if rs.problems:
        raise ValueError(f"{path}: " + "; ".join(rs.problems))
    return counters(rs.results, rs.table_am)


def run(args) -> int:
    one, two = load(args.one), load(args.two)
    diffs = unstable(one, two, args.instructions_tolerance)
    print(f"{len(one)} cells, {sum(len(c.counters) for c in one.values())} counters")
    for cid, m, x, y in diffs:
        print(f"unstable: {cid} {m}: {x} vs {y}")
    if not diffs:
        print("every counter is the same in both runs")
    return int(bool(diffs))
