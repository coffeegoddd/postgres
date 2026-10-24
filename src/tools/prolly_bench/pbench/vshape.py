"""The V suite's growth-shape gate: how each version-control operation's
work grows with table size N and changed rows Δ.

  prolly_bench vshape RUN [--known configs/v_shape_known.toml] [--json OUT]

A V9 cell runs each operation at four or more table sizes (the "rows"
sweep, Δ fixed) and four or more change sizes (the "delta" sweep, N
fixed), each on a fresh database, and counts its work in pg_stat_prolly.
Counters are deterministic, so a shape fitted to them is the code's, not
the machine's.  A counter's growth is its fastest between adjacent
levels, the slope of log(max(value, floor)) against log(level) (growth()),
and its shape the first of

  zero    every value is 0
  flat    growth <= 0.1   (independent of the swept size)
  log     growth <= 0.35  (a tree descent: depth grows as log N)
  linear  growth <= 1.15
  super   anything faster

TARGETS gives each check (operation and sweep) the shape each of its
counters must not exceed: the targets of the constant-time and
logarithmic version-control operations.  A counter that grows faster
fails, unless configs/v_shape_known.toml lists it as a known exception:
the shape it has today and the issue that owns the work to meet the
target.  A known exception is still checked.  It fails when the counter
grows faster than its recorded shape (a regression), when it grows slower
but still misses the target (the entry is to record the better shape), and
when it meets the target (the entry is to be removed: the check is then
enforced).  An entry naming no check, sweep or counter of the run fails
too.  Floors keep work too small to tell from noise out of the fit (a
handful of nodes read either way).
"""

from __future__ import annotations

import json
import math
import sys
import tomllib
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Optional

SHAPES = ("zero", "flat", "log", "linear", "super")
BOUNDS = {"flat": 0.1, "log": 0.35, "linear": 1.15}

# What each operation counts in: the work counters the checks gate.
# (Bytes and cache counters follow the work, or the cache's state.)
TREE = ("nodes_read", "nodes_written", "chunks_appended", "fold_rows_read", "log_pages_read")
MAP = ("bmap_map_pages_copied", "bmap_refcount_updates", "bmap_blocks_freed", "page_copies_shared",
       "bmap_pages_preserved")
APPLY = ("apply_rows", "apply_index_tuples")
WORK = TREE + MAP + APPLY + ("catalog_rows_copied",)
# What an operation reads of the catalogs: block fetches of every system
# catalog and index (not a pg_stat_prolly counter; V9 reads it with them).
CATALOG = ("catalog_blocks_read",)

# Below these, a counter's values are noise to a fit.
FLOORS = {name: 8.0 for name in WORK}
FLOORS["catalog_blocks_read"] = 64.0
FLOORS.update({"nodes_read": 32.0, "bmap_refcount_updates": 64.0, "bmap_blocks_freed": 64.0,
               "page_copies_shared": 64.0, "bmap_pages_preserved": 64.0})


def _all(shape: str, names=WORK) -> dict[str, str]:
    return {n: shape for n in names}


# (check, sweep) -> {counter: the shape it may not exceed}.  rows: N grows,
# Δ fixed; delta: Δ grows, N fixed.
TARGETS: dict[tuple[str, str], dict[str, str]] = {
    # a refs entry: nothing grows with the table or its working set
    ("create_branch", "rows"): _all("flat"),
    ("create_branch", "delta"): _all("flat"),
    # a session's switch to a branch already used: none either
    ("checkout_used", "rows"): _all("flat"),
    ("checkout_used", "delta"): _all("flat"),
    # first use: map work flat, catalog work in the versioned objects only
    ("first_use", "rows"): _all("flat"),
    # moving the head: no rows read
    ("soft_reset", "rows"): _all("flat"),
    ("soft_reset", "delta"): _all("flat"),
    # discarding the working set: no rows written back, map work flat
    ("hard_reset", "rows"): {**_all("zero", APPLY), **_all("flat", MAP)},
    ("hard_reset", "delta"): {**_all("zero", APPLY), **_all("flat", MAP)},
    # letting go of a branch's storage: flat in the foreground
    ("branch_delete", "rows"): _all("flat", MAP),
    ("branch_delete", "delta"): _all("flat", MAP),
    # a snapshot mount: the commit's root items, nothing of the table
    ("mount", "rows"): _all("flat"),
    # a historical point read: a descent of the tree
    ("point_read", "rows"): {"nodes_read": "log"},
    # a commit: tree work of Δ descents, nothing of N
    ("commit", "rows"): _all("log", TREE),
    ("commit", "delta"): _all("linear", TREE),
    # a diff: the differing paths
    ("diff", "rows"): {"nodes_read": "log"},
    ("diff", "delta"): {"nodes_read": "linear"},
    # a merge: the differences examined and written, and the pages kept as
    # of its commit (the map's work of the paths changed)
    ("merge", "rows"): {**_all("log", TREE), **_all("flat", APPLY), **_all("log", MAP)},
    ("merge", "delta"): {**_all("linear", TREE), **_all("linear", APPLY)},
    # a fast-forward merge: the same, but the map pages it copies, which sit
    # on the bound between log and linear (0.35): gated at linear, so the
    # check doesn't flip between met and known with a page more or less
    ("ff_merge", "rows"): {**_all("log", TREE), **_all("flat", APPLY), **_all("log", MAP),
                           "bmap_map_pages_copied": "linear"},
    ("ff_merge", "delta"): {**_all("linear", TREE), **_all("linear", APPLY)},
    # a transaction's own commit: Δ descents of the tree, and the chunks
    # the last one wrote read from the cache, not the files.  (Not Δ: past
    # the changes it keeps, maintenance_work_mem's, it commits the working
    # set, a fold, as it should)
    ("txn_commit", "rows"): {**_all("log", TREE), "chunk_file_reads": "log"},
    # as unrelated objects are added (shared tables, another branch's): a
    # first use's copies, a commit's canonical form and a branch's delete
    # read the branch's catalog rows only
    ("first_use", "catalog"): {**_all("flat", CATALOG), "catalog_rows_copied": "flat"},
    ("commit", "catalog"): _all("flat", CATALOG),
    ("branch_delete", "catalog"): _all("flat", CATALOG),
}


def growth(levels: list[float], values: list[float], floor: float) -> float:
    """A series' fastest growth: the largest slope of log(max(value,
    floor)) on log(level) between adjacent levels.  Not one fit over all
    of them: an operation that changes how it works part way through the
    sweep (a reset that writes rows back while few changed, and shares
    another branch's storage once many did) grows at the fastest of its
    ways, which a fit across the change would average away."""
    if len(levels) < 4 or max(levels) / min(levels) < 8 or list(levels) != sorted(set(levels)):
        raise ValueError("a sweep needs at least four increasing levels spanning 8x")
    ys = [math.log(max(v, floor, 1e-9)) for v in values]
    xs = [math.log(x) for x in levels]
    return max((y2 - y1) / (x2 - x1) for x1, x2, y1, y2 in zip(xs, xs[1:], ys, ys[1:]))


def shape(levels: list[float], values: list[float], floor: float) -> tuple[str, float]:
    """A series' shape and its growth."""
    b = growth(levels, values, floor)
    if all(v == 0 for v in values):
        return "zero", b
    for name in ("flat", "log", "linear"):
        if b <= BOUNDS[name]:
            return name, b
    return "super", b


def rank(name: str) -> int:
    return SHAPES.index(name)


@dataclass
class Known:
    check: str
    sweep: str
    counter: str
    current: str               # the shape it has today
    issue: str                 # the issue owning the work to meet the target
    reason: str


def load_known(path: Optional[Path]) -> list[Known]:
    """v_shape_known.toml's [[known]] entries: check, sweep, counter,
    current, issue and reason, each required."""
    if not path:
        return []
    with open(path, "rb") as f:
        raw = tomllib.load(f)
    if set(raw) - {"known"}:
        raise ValueError(f"{path}: only [[known]] entries belong here")
    out = []
    fields = {"check", "sweep", "counter", "current", "issue", "reason"}
    for e in raw.get("known", []):
        if set(e) != fields or not all(isinstance(v, str) and v.strip() for v in e.values()):
            raise ValueError(f"{path}: an entry needs {', '.join(sorted(fields))}: {e}")
        if e["current"] not in SHAPES:
            raise ValueError(f"{path}: current must be one of {', '.join(SHAPES)}: {e}")
        if not e["issue"].startswith("#"):
            raise ValueError(f"{path}: issue is an issue number, #N: {e}")
        out.append(Known(**{k: e[k].strip() for k in fields}))
    keys = [(k.check, k.sweep, k.counter) for k in out]
    if len(set(keys)) != len(keys):
        raise ValueError(f"{path}: a check's counter is listed twice")
    return out


@dataclass
class Finding:
    check: str
    sweep: str
    counter: str
    target: str
    result: str                # met, unmet, known, known_regressed,
                               # known_improved, known_met, missing,
                               # unmatched
    measured: Optional[str] = None
    growth: Optional[float] = None
    current: Optional[str] = None
    issue: str = ""
    values: Optional[list[float]] = None

    @property
    def fails(self) -> bool:
        return self.result not in ("met", "known")


def judge(check: str, sweep: str, counter: str, target: str,
          levels: list[float], values: Optional[list[float]],
          known: Optional[Known]) -> Finding:
    if values is None:
        return Finding(check, sweep, counter, target, "missing",
                       current=known.current if known else None,
                       issue=known.issue if known else "")
    measured, b = shape(levels, values, FLOORS.get(counter, 8.0))
    f = Finding(check, sweep, counter, target, "met", measured, b, values=values)
    met = rank(measured) <= rank(target)
    if known is None:
        f.result = "met" if met else "unmet"
        return f
    f.current, f.issue = known.current, known.issue
    if met:
        f.result = "known_met"
    elif rank(measured) > rank(known.current):
        f.result = "known_regressed"
    elif rank(measured) < rank(known.current):
        f.result = "known_improved"
    else:
        f.result = "known"
    return f


def gate(cells: list[dict], known: list[Known]) -> list[Finding]:
    """Findings of every V9 cell's series (its raw v9-series.json)."""
    out = []
    for series in cells:
        for (check, sweep), counters in TARGETS.items():
            sw = series.get(sweep)
            if sw is None:
                continue
            ran = sw["checks"].get(check)
            for counter, target in counters.items():
                k = next((e for e in known if (e.check, e.sweep, e.counter) ==
                          (check, sweep, counter)), None)
                values = None if ran is None else ran.get(counter)
                out.append(judge(check, sweep, counter, target, sw["levels"], values, k))
    named = {(f.check, f.sweep, f.counter) for f in out}
    for k in known:
        if (k.check, k.sweep, k.counter) not in named:
            out.append(Finding(k.check, k.sweep, k.counter, "", "unmatched",
                               current=k.current, issue=k.issue))
    return out


EXPLAIN = {
    "unmet": "grows faster than its target",
    "known_regressed": "a known exception grows faster than its recorded shape",
    "known_improved": "a known exception grows slower than recorded: record its shape",
    "known_met": "a known exception meets its target: remove its entry, enforcing the check",
    "missing": "the server doesn't report the counter",
    "unmatched": "the entry names no check, sweep or counter of the run",
}


def series_of(run_dir: Path) -> list[dict]:
    """Each V9 cell's series, from its raw directory."""
    out = []
    for line in (run_dir / "results.jsonl").read_text().splitlines():
        if not line.strip():
            continue
        r = json.loads(line)
        if r["key"]["workload"] != "V9":
            continue
        if r.get("status") not in (None, "ok"):
            raise SystemExit(f"V9 cell {r['key']} did not complete: {r.get('status')}")
        out.append(json.loads((run_dir / r["raw_dir"] / "v9-series.json").read_text()))
    return out


def table(findings: list[Finding]) -> str:
    lines = ["| Check | Sweep | Counter | Target | Measured | Growth | Values | Result | Owner |",
             "|---|---|---|---|---|---|---|---|---|"]
    for f in findings:
        growth_text = "" if f.growth is None else f"{f.growth:.2f}"
        values = ", ".join(f"{v:,}" for v in f.values) if f.values else ""
        lines.append(f"| {f.check} | {f.sweep} | {f.counter} | {f.target} | {f.measured or ''} "
                     f"| {growth_text} | {values} | {f.result} | {f.issue} |")
    return "\n".join(lines) + "\n"


def run(args) -> int:
    run_dir: Path = args.dir
    if args.known is not None and not args.known.exists():
        raise SystemExit(f"{args.known}: no such file")
    known = load_known(args.known)
    cells = series_of(run_dir)
    if not cells:
        print("V shape: no V9 cells in the run", file=sys.stderr)
        return 1
    findings = gate(cells, known)
    failed = [f for f in findings if f.fails]
    for f in failed:
        print(f"{f.result.upper()} {f.check}/{f.sweep} {f.counter}: target {f.target or '-'}, "
              f"measured {f.measured or '-'}"
              + (f" (growth {f.growth:.2f})" if f.growth is not None else "")
              + (f", recorded {f.current} ({f.issue})" if f.current else "")
              + f": {EXPLAIN[f.result]}", file=sys.stderr)
    if args.json:
        args.json.write_text(json.dumps({"passed": not failed,
                                         "findings": [asdict(f) for f in findings]},
                                        indent=2) + "\n")
    if args.markdown:
        args.markdown.write_text(table(findings))
    met = sum(f.result == "met" for f in findings)
    exceptions = sum(f.result == "known" for f in findings)
    print(f"V shape: {'passed' if not failed else 'FAILED'}: {met} met, {exceptions} known "
          f"exceptions, {len(failed)} failing")
    return 0 if not failed else 1
