"""The V suite's growth gate: per-commit work that grows with the number of
commits made before it.

  prolly_bench vgrowth RUN [--bound 1.5] [--accept configs/v_growth_accept.toml] [--json OUT]

A V7 cell runs K transactions, each followed by a commit, and records each
counter's mean per transaction over the first and the last tenth of them
(first_<counter>, last_<counter>).  A linear path's means stay close; a
quadratic one's last mean is about 9 times its first at V7's sizes.  A
counter fails when its last mean over its first passes the bound and the
last mean is above its floor (work too small to count either way passes).

configs/v_growth_accept.toml lists growth that is known and has an issue
to fix it, one [[accept]] entry each (workload "V7/<variant>", engine,
counter, reason), in perf_accept.toml's format.  Unlike perf_accept.toml's
entries, these stand until removed: the fix that ends the growth removes
its entries, and an accepted counter that no longer grows is reported, so
that it is.
"""

from __future__ import annotations

import json
import math
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Optional

from pbench.sgate import Acceptance, _finite, load_accept

# The counters V7 records, and below what mean per transaction growth
# doesn't count (a counter that is 0 at first and 0.5 later isn't a path
# gone quadratic).
FLOORS = {
    "fold_rows_read": 2.0,
    "fold_candidates": 2.0,
    "fold_full_tables": 0.0,
    "nodes_read": 4.0,
    "chunks_appended": 2.0,
    "wal_bytes": 4096.0,
    # (every relation's buffers a client read or hit: the change log's walk
    # among them, dead entries and all)
    "buffer_accesses": 64.0,
}

# Those read from the server's views; the rest are pg_stat_prolly's.
SERVER_COUNTERS = {"wal_bytes", "buffer_accesses"}

DEFAULT_BOUND = 1.5


def ratio(first: float, last: float) -> float:
    """last over first; 1 when both are 0, infinite when only first is."""
    if first == 0:
        return 1.0 if last == 0 else math.inf
    return last / first


def means(series: list[float], fraction: float = 0.1) -> tuple[float, float]:
    """The mean of a per-transaction series' first and last fraction."""
    k = max(1, int(len(series) * fraction))
    if len(series) < 2 * k:
        raise ValueError("a series needs at least two tenths that don't overlap")
    return sum(series[:k]) / k, sum(series[-k:]) / k


@dataclass
class Finding:
    workload: str              # "V7/<variant>"
    engine: str
    counter: str
    result: str                # flat, grows, accepted, accepted_flat, missing
    first: Optional[float] = None
    last: Optional[float] = None
    ratio: Optional[float] = None
    reason: str = ""           # an accepted one's

    @property
    def fails(self) -> bool:
        return self.result in ("grows", "missing")


def judge(workload: str, engine: str, counter: str, first: Optional[float],
          last: Optional[float], bound: float,
          accept: list[Acceptance]) -> Finding:
    if first is None or last is None:
        return Finding(workload, engine, counter, "missing", first, last)
    r = ratio(first, last)
    grows = r > bound and last > FLOORS.get(counter, 0.0)
    known = next((a for a in accept if a.matches(workload, engine, counter)), None)
    if known:
        result = "accepted" if grows else "accepted_flat"
    else:
        result = "grows" if grows else "flat"
    return Finding(workload, engine, counter, result, first, last, r,
                   known.reason if known else "")


def gate(results: list[dict], bound: float, accept: list[Acceptance],
         engine: str) -> list[Finding]:
    """Findings of every V7 cell of a run's results.jsonl rows."""
    out = []
    for r in results:
        k = r["key"]
        if k["workload"] != "V7":
            continue
        name = f"V7/{k['variant']}" if k["variant"] else "V7"
        m = r.get("metrics", {})
        for counter in FLOORS:
            out.append(judge(name, engine, counter, m.get(f"first_{counter}"),
                             m.get(f"last_{counter}"), bound, accept))
    return out


def run(args) -> int:
    run_dir: Path = args.dir
    results = [json.loads(line) for line in
               (run_dir / "results.jsonl").read_text().splitlines() if line.strip()]
    engine = json.loads((run_dir / "run.json").read_text())["table_am"]
    accept = load_accept(args.accept)
    findings = gate(results, args.bound, accept, engine)
    if not findings:
        print("V growth: no V7 cells in the run", file=sys.stderr)
        return 1
    failed = [f for f in findings if f.fails]
    for f in findings:
        if f.result in ("grows", "missing", "accepted_flat"):
            print(f"{f.result.upper()} {f.workload} {f.engine} {f.counter}: "
                  f"{f.first} -> {f.last} per transaction"
                  + (f" ({f.ratio:.2f}x)" if f.ratio is not None and math.isfinite(f.ratio) else ""),
                  file=sys.stderr)
    out = {"bound": args.bound, "passed": not failed,
           "findings": [asdict(f) for f in findings]}
    if args.json:
        args.json.write_text(json.dumps(_finite(out), indent=2, allow_nan=False) + "\n")
    print(f"V growth: {'passed' if not failed else 'FAILED'}")
    return 0 if not failed else 1
