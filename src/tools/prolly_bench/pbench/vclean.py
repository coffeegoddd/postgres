"""Cleanup invariance (V10): the same queries read the same rows, and about
as fast, after storage cleanup as before it.

A V10 cell reads a query set Q, R times, before the cleanups (phase 1) and
after them (phase 2), recording each read's result digest and latency.
Every digest must be what it was the first time: the workload fails the
cell on any difference (a read that depended on something a cleanup is
allowed to free).  Latency is compared pairwise: repetition i of phase 2
over repetition i of phase 1, the median of those ratios per query, for
prolly's queries and for the heap tables beside them, which show what
cleanup costs a table without version control.

This module holds what the workload and the report share: the session
script one pass of Q runs as, its parsing, the digests' comparison and
the latency judgment.  `prolly_bench vclean RUN` reports every V10 cell's
queries, and with --gate-latency fails on a query over the tolerance.
"""

from __future__ import annotations

import json
import re
import statistics
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Optional

# A query of Q: (name, kind, sql).  "read" is digested and timed, "op" (a
# checkout, a reset, a change it discards) timed only, "set" (a setting
# for the reads after it) neither.
KINDS = ("read", "op", "set")

# A query is slower when its paired ratio passes its tolerance and its
# phase 2 median is more than FLOOR_MS over phase 1's.  Its tolerance is
# TOLERANCE, or more for a query whose phase 1 repetitions spread: 1 plus
# NOISE_K times their median absolute deviation over their median (so a
# query that moves 10% between repetitions before any cleanup isn't
# called slower for moving as much after).
TOLERANCE = 1.10
FLOOR_MS = 1.0
NOISE_K = 3.0

_TIME_RE = re.compile(r"^Time: ([\d.]+) ms", re.M)
_DIGEST_RE = re.compile(r"^v10\|(\w+)\|(\d+)\|(\w*)$", re.M)


def digest_sql(name: str, select: str) -> str:
    """A statement giving select's row count and a digest of its rows,
    whatever their order."""
    return (f"SELECT 'v10|{name}|' || count(*) || '|' || "
            f"coalesce(md5(string_agg(r::text, E'\\n' ORDER BY r::text)), '') "
            f"FROM ({select}) r;")


def pass_script(queries: list[tuple[str, str, str]]) -> str:
    """One pass of Q as one psql session: each read run with its rows
    discarded (timed: what a client waits for, first, so that a cold pass
    times it cold), then digested (untimed); each op timed with its output
    discarded."""
    out = ["\\timing on"]
    for name, kind, sql in queries:
        if kind not in KINDS:
            raise ValueError(f"unknown query kind {kind!r} of {name}")
        if kind == "set":
            out += ["\\timing off", f"{sql};", "\\timing on"]
        elif kind == "read":
            out += ["\\o /dev/null", f"{sql};", "\\o",
                    "\\timing off", digest_sql(name, sql), "\\timing on"]
        else:
            out += ["\\o /dev/null", f"{sql};", "\\o"]
    return "\n".join(out) + "\n"


def timed(queries: list[tuple[str, str, str]]) -> list[str]:
    """The names of the timed queries, in order."""
    return [name for name, kind, _ in queries if kind != "set"]


def parse_pass(stdout: str, queries: list[tuple[str, str, str]]
               ) -> tuple[dict[str, str], dict[str, float]]:
    """A pass's digests ({read: "rows|md5"}) and latencies ({query: ms})."""
    digests = {m.group(1): f"{m.group(2)}|{m.group(3)}" for m in _DIGEST_RE.finditer(stdout)}
    reads = [name for name, kind, _ in queries if kind == "read"]
    if sorted(digests) != sorted(reads):
        raise RuntimeError(f"V10 expected digests of {reads}, got {sorted(digests)}")
    names = timed(queries)
    times = [float(t) for t in _TIME_RE.findall(stdout)]
    if len(times) != len(names):
        raise RuntimeError(f"V10 expected {len(names)} timings, got {len(times)}")
    return digests, dict(zip(names, times))


def mismatches(base: dict[str, str], other: dict[str, str]) -> list[str]:
    """The queries whose digest differs from base's, or is missing."""
    return sorted(q for q in set(base) | set(other) if base.get(q) != other.get(q))


def paired_ratio(p1: list[float], p2: list[float]) -> Optional[float]:
    """The median of repetition i's phase 2 latency over its phase 1's."""
    if len(p1) != len(p2) or not p1:
        raise ValueError("paired latencies need as many repetitions in each phase")
    ratios = [b / a for a, b in zip(p1, p2) if a > 0]
    return statistics.median(ratios) if ratios else None


@dataclass
class Finding:
    cell: str
    query: str
    engine: str             # "prolly", or "heap" for the heap tables
    p1_ms: float            # phase 1's median
    p2_ms: float
    ratio: Optional[float]  # paired_ratio()
    tolerance: float        # the query's own: tolerance_of()
    p1_min_ms: float        # phase 1's spread
    p1_max_ms: float
    slower: bool            # over its tolerance, by more than the floor


def tolerance_of(p1: list[float], tolerance: float = TOLERANCE, k: float = NOISE_K) -> float:
    """A query's tolerance: the fixed one, or its phase 1 noise's."""
    med = statistics.median(p1)
    if med <= 0:
        return tolerance
    mad = statistics.median(abs(x - med) for x in p1)
    return max(tolerance, 1 + k * mad / med)


def judge(cell: str, query: str, p1: list[float], p2: list[float],
          tolerance: float = TOLERANCE, floor_ms: float = FLOOR_MS) -> Finding:
    ratio = paired_ratio(p1, p2)
    a, b = statistics.median(p1), statistics.median(p2)
    tol = tolerance_of(p1, tolerance)
    slower = ratio is not None and ratio > tol and b - a > floor_ms
    return Finding(cell, query, "heap" if query.startswith("heap_") else "prolly",
                   a, b, ratio, tol, min(p1), max(p1), slower)


def findings(series: dict, cell: str, tolerance: float = TOLERANCE,
             floor_ms: float = FLOOR_MS) -> list[Finding]:
    """Every timed query's finding, from a cell's v10-series.json."""
    p1, p2 = series["phase1"]["latency_ms"], series["phase2"]["latency_ms"]
    return [judge(cell, q, p1[q], p2[q], tolerance, floor_ms) for q in p1]


def metrics(series: dict, tolerance: float = TOLERANCE,
            floor_ms: float = FLOOR_MS) -> dict[str, Optional[float]]:
    """A cell's metrics: each query's phase medians and paired ratio, and
    per engine the median ratio and the queries slower."""
    out: dict[str, Optional[float]] = {}
    fs = findings(series, "", tolerance, floor_ms)
    for f in fs:
        out[f"{f.query}_p1_ms"] = f.p1_ms
        out[f"{f.query}_p2_ms"] = f.p2_ms
        out[f"{f.query}_ratio"] = f.ratio
    for engine in ("prolly", "heap"):
        mine = [f for f in fs if f.engine == engine]
        ratios = [f.ratio for f in mine if f.ratio is not None]
        out[f"{engine}_ratio_median"] = statistics.median(ratios) if ratios else None
        out[f"{engine}_queries_slower"] = float(sum(f.slower for f in mine))
    return out


def table(fs: list[Finding]) -> str:
    lines = ["| Cell | Query | Engine | Phase 1 ms | Phase 1 range | Phase 2 ms | Paired ratio "
             "| Tolerance | Result |",
             "|---|---|---|---|---|---|---|---|---|"]
    for f in fs:
        ratio = "" if f.ratio is None else f"{f.ratio:.3f}"
        lines.append(f"| {f.cell} | {f.query} | {f.engine} | {f.p1_ms:.3f} "
                     f"| {f.p1_min_ms:.3f}-{f.p1_max_ms:.3f} | {f.p2_ms:.3f} "
                     f"| {ratio} | {f.tolerance:.3f} | {'slower' if f.slower else 'ok'} |")
    return "\n".join(lines) + "\n"


def series_of(run_dir: Path) -> list[tuple[str, dict]]:
    """Each V10 cell's (name, series); a cell that did not complete (its
    digests differed, or a check after the cleanups failed) fails."""
    out = []
    for line in (run_dir / "results.jsonl").read_text().splitlines():
        if not line.strip():
            continue
        r = json.loads(line)
        if r["key"]["workload"] != "V10":
            continue
        if r.get("status") not in (None, "ok"):
            raise SystemExit(f"V10 cell {r['key']} did not complete: {r.get('status')}: "
                             f"{r.get('message', '')}")
        name = "/".join(str(r["key"][k]) for k in ("size", "variant", "rep"))
        out.append((name, json.loads((run_dir / r["raw_dir"] / "v10-series.json").read_text())))
    return out


def run(args) -> int:
    cells = series_of(args.dir)
    if not cells:
        print("V cleanup: no V10 cells in the run", file=sys.stderr)
        return 1
    fs = [f for name, s in cells for f in findings(s, name, args.tolerance, args.floor_ms)]
    slower = [f for f in fs if f.slower]
    for f in slower:
        print(f"SLOWER {f.cell} {f.query} ({f.engine}): {f.p1_ms:.3f} ms -> {f.p2_ms:.3f} ms, "
              f"paired ratio {f.ratio:.3f}", file=sys.stderr)
    if args.json:
        args.json.write_text(json.dumps({"slower": len(slower),
                                         "findings": [asdict(f) for f in fs]}, indent=2) + "\n")
    if args.markdown:
        args.markdown.write_text(table(fs))
    failed = bool(slower) and args.gate_latency
    print(f"V cleanup: digests identical in {len(cells)} cells; {len(slower)} of {len(fs)} "
          f"query latencies slower after cleanup (tolerance at least {args.tolerance}, floor "
          f"{args.floor_ms} ms){'' if args.gate_latency else ', reported only'}")
    return 1 if failed else 0
