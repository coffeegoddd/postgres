"""The V suite's growth gate: per-commit work that grows with the number of
commits made before it.

  prolly_bench vgrowth RUN [--bound 1.5] [--accept configs/v_growth_accept.toml] [--json OUT]

A V7 cell runs K transactions, each followed by a commit, and records each
counter's mean per transaction over the first and the last tenth of them
(first_<counter>, last_<counter>).  A linear path's means stay close; a
quadratic one's last mean is about 9 times its first at V7's sizes.  A
counter fails when its last mean over its first passes the bound and the
last mean is above its floor (work too small to count either way passes).

Work done once every so many transactions (a root captured each interval)
makes a counter's series periodic: a few transactions cost many times the
rest.  A tenth then holds one more or one fewer of them by where they fall,
and its mean says so, not growth.  So where a series has such events (at
least MIN_EVENTS transactions, and no more than MAX_EVENT_SHARE of them,
above EVENT_FACTOR times its median), it is judged twice, and grows if
either does: its baseline (the series without the events) by its tenths,
as any other series; and the events' cost per transaction (their mean
size over their mean spacing) over the first fifth of them against the
last, so that events growing larger or coming more often both show.
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
    # (every relation's buffers a client read or hit)
    "buffer_accesses": 64.0,
    # (the change logs' pages folds walked, dead entries' too: what a held
    # horizon grows)
    "log_pages_read": 2.0,
}

# Counters that count every process's work (autovacuum's collections and
# moves, its WAL): where a variant runs autovacuum, a worker's pass in one
# tenth and not the other can pass the bound.  vgrowth reports them there
# (the cell's ungated_<counter> metric), and gates the rest.
AUTOVACUUM_COUNTED = {"wal_bytes", "nodes_read", "chunks_appended"}

# Those read from the server's views; the rest are pg_stat_prolly's.
SERVER_COUNTERS = {"wal_bytes", "buffer_accesses"}

DEFAULT_BOUND = 1.5


def ratio(first: float, last: float) -> float:
    """last over first; 1 when both are 0, infinite when only first is."""
    if first == 0:
        return 1.0 if last == 0 else math.inf
    return last / first


def growth_metrics(series: dict[str, Optional[list[float]]],
                   ungated: set[str] = frozenset()) -> dict[str, Optional[float]]:
    """A V7 cell's metrics from its per-transaction series: first_ and
    last_<counter> (None for a counter the server didn't report), growth_
    (None where infinite: results hold finite numbers only), and
    ungated_<counter> = 1 for each counter not gated."""
    out: dict[str, Optional[float]] = {}
    for c, s in series.items():
        if s is None:
            out[f"first_{c}"] = out[f"last_{c}"] = out[f"growth_{c}"] = None
            continue
        first, last = means(s)
        r = ratio(first, last)
        out[f"first_{c}"], out[f"last_{c}"] = first, last
        out[f"growth_{c}"] = r if math.isfinite(r) else None
    for c in ungated:
        out[f"ungated_{c}"] = 1.0
    return out


# A transaction costing this many times the series' median is a periodic
# event's (see the module's docstring), if there are this many of them,
# and they are no more than this share of the series (more are no events,
# but the work it does).
EVENT_FACTOR = 10.0
MIN_EVENTS = 3
MAX_EVENT_SHARE = 0.05


def events(series: list[float]) -> list[int]:
    """The indexes of a series' periodic events: none if it has fewer than
    MIN_EVENTS or more than MAX_EVENT_SHARE of it, or its median is 0."""
    ordered = sorted(series)
    median = ordered[len(ordered) // 2]
    if median <= 0:
        return []
    at = [i for i, v in enumerate(series) if v > EVENT_FACTOR * median]
    if len(at) < MIN_EVENTS or len(at) > MAX_EVENT_SHARE * len(series):
        return []
    return at


def tenths(series: list[float], fraction: float = 0.1) -> tuple[float, float]:
    """The mean of a series' first and last fraction."""
    k = max(1, int(len(series) * fraction))
    if len(series) < 2 * k:
        raise ValueError("a series needs at least two tenths that don't overlap")
    return sum(series[:k]) / k, sum(series[-k:]) / k


def event_costs(series: list[float], at: list[int]) -> tuple[float, float]:
    """The events' cost per transaction, mean size over mean spacing, over
    their first fifth and their last (each at least two events)."""
    m = max(2, len(at) // 5)
    first, last = at[:m], at[-m:]

    def cost(group):
        size = sum(series[i] for i in group) / len(group)
        spacing = (group[-1] - group[0]) / (len(group) - 1)
        return size / max(spacing, 1.0)
    return cost(first), cost(last)


def means(series: list[float], fraction: float = 0.1) -> tuple[float, float]:
    """A per-transaction series' first and last means: its tenths'; or, for
    a periodic one, its baseline's tenths' or its events' costs, whichever
    grows more."""
    at = events(series)
    if not at:
        return tenths(series, fraction)
    skip = set(at)
    base = tenths([v for i, v in enumerate(series) if i not in skip], fraction)
    ev = event_costs(series, at)
    return max(base, ev, key=lambda p: ratio(*p))


@dataclass
class Finding:
    workload: str              # "V7/<variant>"
    engine: str
    counter: str
    result: str                # flat, grows, accepted, accepted_flat
                               # (accepted, no longer growing: its entry
                               # is to be removed), ungated, missing,
                               # unmatched (an entry naming no finding)
    first: Optional[float] = None
    last: Optional[float] = None
    ratio: Optional[float] = None
    reason: str = ""           # an accepted one's

    @property
    def fails(self) -> bool:
        return self.result in ("grows", "missing", "accepted_flat", "unmatched")


def judge(workload: str, engine: str, counter: str, first: Optional[float],
          last: Optional[float], bound: float,
          accept: list[Acceptance], gated: bool = True) -> Finding:
    if first is None or last is None:
        return Finding(workload, engine, counter, "missing", first, last)
    r = ratio(first, last)
    if not gated:
        return Finding(workload, engine, counter, "ungated", first, last, r)
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
                             m.get(f"last_{counter}"), bound, accept,
                             gated=not m.get(f"ungated_{counter}")))
    # (an entry that names no finding: a variant or counter renamed)
    named = {(f.workload, f.engine, f.counter) for f in out}
    for a in accept:
        if (a.workload, a.engine, a.counter) not in named and a.engine == engine:
            out.append(Finding(a.workload, a.engine, a.counter, "unmatched",
                               reason=a.reason))
    return out


def run(args) -> int:
    run_dir: Path = args.dir
    results = [json.loads(line) for line in
               (run_dir / "results.jsonl").read_text().splitlines() if line.strip()]
    engine = json.loads((run_dir / "run.json").read_text())["table_am"]
    if args.accept is not None and not args.accept.exists():
        raise SystemExit(f"{args.accept}: no such file")
    accept = load_accept(args.accept)
    findings = gate(results, args.bound, accept, engine)
    if not findings:
        print("V growth: no V7 cells in the run", file=sys.stderr)
        return 1
    failed = [f for f in findings if f.fails]
    for f in findings:
        if f.result in ("grows", "missing", "accepted_flat", "unmatched"):
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
