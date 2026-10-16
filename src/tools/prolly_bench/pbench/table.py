"""Side-by-side tables of several result sets: one column per engine.

  prolly_bench table                       latest result set per label
  prolly_bench table DIR DIR ...           exactly these result sets

For each metric, one table: a row per (workload, variant, size, clients),
a column per engine, each cell the median over repetitions.  In the S suite
every number is followed by its ratio to the baseline (B0, stock heap) when
a baseline is present.  Writes Markdown (also printed) and a standalone HTML
page next to the result sets.
"""

from __future__ import annotations

import html
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from pbench.core import registry
from pbench.core.stats import summarize, paired_ratio_interval
from pbench.report import HIGHER_IS_BETTER, load_results
from pbench.core.validation import cell_problems, compatible, validate_run

# (metric, heading, unit) per suite, in display order.  "throughput" is
# tps, or rows/s for row-oriented cells (S4, sysbench bulk_insert).
S_TABLES = [
    ("throughput", "Throughput", "tps, or rows/s for bulk loads"),
    ("latency_p50_ms", "Latency p50", "ms"),
    ("latency_p99_ms", "Latency p99", "ms"),
    ("latency_p999_ms", "Latency p99.9", "ms"),
    ("wal_bytes_per_operation", "WAL per completed unit", "bytes"),
    ("device_write_bytes_per_operation", "Whole-device writes per completed unit", "bytes"),
    ("host_busy_cpu_seconds", "Host busy CPU", "seconds"),
    ("latency_samples", "Latency observations", "samples"),
    ("failed_transactions", "Failed transactions", "count"),
    ("skipped_transactions", "Requests skipped under scheduled load", "count"),
    ("hot_query_seconds", "TPC-H measured hot queries", "seconds for Q1–Q22"),
    ("client_process_qps", "TPC-H process throughput including cold passes", "queries/s"),
    ("io_hit_ratio", "Buffer hit ratio", "fraction"),
    ("cluster_allocated_bytes_including_wal", "Cluster allocated storage including WAL", "bytes"),
    ("relation_bytes", "Relation storage (excludes engine-global stores)", "bytes"),
    ("vacuum_seconds", "VACUUM after the run", "seconds"),
    ("process_restart_seconds", "Restart after process crash, warm OS cache", "seconds"),
]
V_TABLES = [
    ("branch_create_ms", "Branch create", "ms"),
    ("checkout_ms", "Checkout", "ms"),
    ("commit_ms", "Commit", "ms"),
    ("diff_ms", "Diff", "ms"),
    ("merge_ms", "Merge", "ms"),
    ("clone_ms", "Clone", "ms"),
    ("fetch_ms", "Fetch", "ms"),
    ("push_ms", "Push", "ms"),
    ("background_tps", "Background S1 throughput", "tps"),
]
_HIGHER = HIGHER_IS_BETTER | {"throughput"}
# Metrics that measure write volume; not applicable to read-only cells.
_WRITE_METRICS = {"wal_bytes_per_operation", "device_write_bytes_per_operation"}
_STATUS_TEXT = {"not_implemented": "not impl.", "unavailable": "unavail.",
                "error": "error", "not_applicable": "n/a"}


@dataclass
class ResultSet:
    path: Path
    suite: str
    label: str
    table_am: str
    started: str
    results: list[dict]
    problems: list[str] = field(default_factory=list)

    @property
    def is_baseline(self) -> bool:
        return self.table_am == "heap" and self.label.startswith("B0")


def load_set(path: Path) -> ResultSet:
    run = json.loads((path / "run.json").read_text())
    problems = validate_run(path)
    try:
        results = load_results(path)
    except (OSError, ValueError, KeyError):
        results = []  # Keep incomplete evidence visible in discovery and reports.
    return ResultSet(path, run["suite"], run["label"], run["table_am"],
                     run["started"], results, problems)


def discover(root: Path, suite: str) -> list[ResultSet]:
    """The most recent result per label, including failed/incomplete runs."""
    latest: dict[str, ResultSet] = {}
    for run_json in sorted(root.glob("*/run.json")):
        rs = load_set(run_json.parent)
        if rs.suite != suite:
            continue
        if rs.label not in latest or rs.started > latest[rs.label].started:
            latest[rs.label] = rs
    return list(latest.values())


def _order(sets: list[ResultSet]) -> list[ResultSet]:
    """Baselines first, then by label."""
    if len({s.label for s in sets}) != len(sets):
        raise ValueError("result labels must be unique; refusing to combine distinct runs implicitly")
    return sorted(sets, key=lambda s: (not s.is_baseline, s.label))


def _metric(r: dict, metric: str) -> Optional[float]:
    m = r["metrics"]
    if metric == "throughput":
        return m.get("tps") if m.get("tps") is not None else m.get("rows_per_second")
    return m.get(metric)


@dataclass
class Cell:
    text: str                    # the value, or a status
    ratio: Optional[float] = None
    better: Optional[bool] = None
    detail: str = ""             # spread and repetitions, for HTML tooltips
    ok: bool = True


def _cell(rs: list[dict], metric: str, base: Optional[list[dict]]) -> Cell:
    if not rs:
        return Cell("", ok=False)
    ok = [r for r in rs if r["status"] == "ok"]
    invalid = [r for r in rs if cell_problems(r)]
    if invalid:
        return Cell(f"invalid {len(invalid)}/{len(rs)}", ok=False,
                    detail="; ".join(p for r in invalid for p in cell_problems(r)))
    if not ok:
        status = rs[0]["status"]
        return Cell(_STATUS_TEXT.get(status, status), ok=False,
                    detail=rs[0].get("message", ""))
    if metric in _WRITE_METRICS and all(r.get("read_only") for r in ok):
        return Cell(_STATUS_TEXT["not_applicable"], ok=False,
                    detail="read-only workload")
    s = summarize(_metric(r, metric) for r in ok)
    if s["n"] != len(rs):
        return Cell(f"missing {len(rs) - s['n']}/{len(rs)}", ok=False)
    if s["median"] is None:
        return Cell("-", ok=False)
    cell = Cell(_fmt(s["median"]),
                detail=f"median of {s['n']}; min {_fmt(s['min'])}, max {_fmt(s['max'])}")
    if base and not any(cell_problems(r) for r in base):
        b = summarize(_metric(r, metric) for r in base if r["status"] == "ok")
        if b["median"] and b["n"] == len(base):
            cell.ratio = s["median"] / b["median"]
            # A point ratio is descriptive, not evidence of a performance
            # difference. Do not color noisy or single-repetition results.
            by_rep = {r.get("key", {}).get("rep"): r for r in base}
            reps = [r.get("key", {}).get("rep") for r in rs]
            if (len(set(reps)) == len(rs) == len(base) and set(reps) == by_rep.keys()
                    and all(r.get("measurement", {}).get("quality") == "benchmark" for r in rs + base)
                    and all(r["measurement"].get("seed") == by_rep[r["key"]["rep"]]["measurement"].get("seed") for r in rs)):
                interval = paired_ratio_interval([(_metric(r, metric), _metric(by_rep[r["key"]["rep"]], metric)) for r in rs])
                if interval:
                    estimate, low, high = interval
                    cell.ratio = estimate
                    cell.detail += f"; paired median ratio 95% bootstrap CI [{low:.3f}, {high:.3f}]"
                    cell.text += f" [95% CI {low:.2f}–{high:.2f}x]"
                    if low > 1.05 or high < 0.95:
                        cell.better = (low > 1.05) == (metric in _HIGHER)
                    else:
                        cell.detail += "; inconclusive at 5% practical effect threshold"
    return cell


def _fmt(x: float) -> str:
    if x == 0:
        return "0"
    if abs(x) >= 100:
        return f"{x:,.0f}"
    if abs(x) >= 1:
        return f"{x:.2f}"
    return f"{x:.3g}"


def _row_keys(sets: list[ResultSet]) -> list[tuple]:
    keys = {(r["key"]["workload"], r["key"]["variant"], r["key"]["size"],
             r["key"]["clients"]) for s in sets for r in s.results}
    return sorted(keys, key=lambda k: (int(k[0][1:]), k[1], k[2], k[3]))


def build(sets: list[ResultSet], suite: str):
    """[(metric, heading, unit, columns, rows)]; rows are (key, [Cell])."""
    registry.load_all()
    sets = _order(sets)
    base = next((s for s in sets if s.is_baseline), None) if suite == "S" else None
    index = {}
    for s in sets:
        for r in s.results:
            k = r["key"]
            index.setdefault((s.label, k["workload"], k["variant"], k["size"],
                              k["clients"]), []).append(r)
    tables = []
    comparability = {s.label: not compatible(base.path, s.path)
                     for s in sets if base and s is not base}
    for metric, heading, unit in (S_TABLES if suite == "S" else V_TABLES):
        rows = []
        for key in _row_keys(sets):
            b = index.get((base.label, *key)) if base else None
            cells = [Cell("INVALID", ok=False, detail="; ".join(s.problems)) if s.problems else
                     _cell(index.get((s.label, *key), []), metric,
                           b if comparability.get(s.label) else None) for s in sets]
            if any(c.ok for c in cells) or metric == tables_first_metric(suite):
                rows.append((key, cells))
        if rows:
            tables.append((metric, heading, unit, sets, rows))
    return tables


def tables_first_metric(suite: str) -> str:
    return (S_TABLES if suite == "S" else V_TABLES)[0][0]


def _header(s: ResultSet) -> str:
    return s.label if s.label == s.table_am else f"{s.label} ({s.table_am})"


def _one_size(sets: list[ResultSet]) -> bool:
    return len({r["key"]["size"] for s in sets for r in s.results}) <= 1


def _workload_title(wid: str) -> str:
    try:
        return f"{wid} {registry.workloads.get(wid).title}"
    except KeyError:
        return wid


# ---------------------------------------------------------------------------
# Markdown
# ---------------------------------------------------------------------------

def to_markdown(sets: list[ResultSet], suite: str) -> str:
    sets = _order(sets)
    out = [f"# {suite} suite by engine\n"]
    out.append("Columns: " + "; ".join(
        f"**{s.label}** = `{s.table_am}` ({s.path.name})" for s in sets) + "\n")
    for s in sets:
        out.extend(f"- {s.label}: {p}" for p in s.problems)
    base = next((s for s in sets if s.is_baseline), None)
    if base:
        for s in sets:
            if s is not base:
                out.extend(f"- {s.label}: comparison withheld: {p}" for p in compatible(base.path, s.path))
    if suite == "S" and any(s.is_baseline for s in sets):
        out.append("Each number is the median over repetitions; the ratio to B0 "
                   "follows it (for throughput and hit ratio higher is better, "
                   "for everything else lower is better).\n")
    for metric, heading, unit, cols, rows in build(sets, suite):
        out.append(f"## {heading} ({unit})\n")
        one_size = _one_size(cols)
        fixed = ["workload", "variant"] + ([] if one_size else ["size"]) + ["clients"]
        out.append("| " + " | ".join(fixed + [_header(s) for s in cols]) + " |")
        out.append("|" + "---|" * (len(fixed) - 1) + "---:|" + "---:|" * len(cols))
        prev = None
        for (w, variant, size, clients), cells in rows:
            texts = []
            for c in cells:
                t = c.text
                if c.ratio is not None:
                    t += f" ({c.ratio:.2f}x)"
                texts.append(t)
            lead = [_workload_title(w) if w != prev else "", variant or "-"]
            lead += [] if one_size else [size]
            prev = w
            out.append("| " + " | ".join(lead + [str(clients)] + texts) + " |")
        out.append("")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# HTML
# ---------------------------------------------------------------------------

_CSS = """
:root { --bg:#fff; --fg:#1d1d1f; --muted:#6e6e73; --line:#e3e3e8; --head:#f5f5f7;
        --good:#1a7f37; --good-bg:#e6f4ea; --bad:#c62828; --bad-bg:#fdecea; --na:#8e8e93; }
@media (prefers-color-scheme: dark) {
  :root { --bg:#161618; --fg:#f2f2f7; --muted:#a1a1a6; --line:#2c2c30; --head:#1f1f23;
          --good:#5fd38a; --good-bg:#16301f; --bad:#ff8a80; --bad-bg:#3a1a1a; --na:#7c7c80; }
}
body { margin:0; padding:24px 16px; background:var(--bg); color:var(--fg);
       font:14px/1.45 -apple-system, system-ui, "Segoe UI", sans-serif; }
main { max-width:1200px; margin:0 auto; }
h1 { font-size:22px; margin:0 0 4px; } h2 { font-size:16px; margin:28px 0 8px; }
.unit { color:var(--muted); font-weight:normal; }
p.meta { color:var(--muted); margin:4px 0 16px; }
nav a { margin-right:12px; color:inherit; } nav { margin-bottom:8px; }
.wrap { overflow-x:auto; border:1px solid var(--line); border-radius:8px; }
table { border-collapse:collapse; width:100%; font-variant-numeric:tabular-nums; }
th, td { padding:6px 10px; border-bottom:1px solid var(--line); white-space:nowrap; }
th { background:var(--head); text-align:left; position:sticky; top:0; }
th.num, td.num { text-align:right; }
td.wl { font-weight:600; }
tr.first td { border-top:2px solid var(--line); }
.ratio { color:var(--muted); font-size:12px; margin-left:6px; }
td.good { background:var(--good-bg); } td.good .ratio { color:var(--good); }
td.bad { background:var(--bad-bg); } td.bad .ratio { color:var(--bad); }
td.na { color:var(--na); font-style:italic; }
"""


def to_html(sets: list[ResultSet], suite: str) -> str:
    sets = _order(sets)
    tables = build(sets, suite)
    esc = html.escape
    parts = [f"<!doctype html><html><head><meta charset='utf-8'>"
             f"<meta name='viewport' content='width=device-width, initial-scale=1'>"
             f"<title>{suite} suite by engine</title><style>{_CSS}</style></head>"
             f"<body><main><h1>{suite} suite by engine</h1>"]
    parts.append("<p class='meta'>" + " · ".join(
        f"<b>{esc(s.label)}</b> = {esc(s.table_am)} ({esc(s.path.name)})" for s in sets)
        + ("<br>Median over repetitions; the ratio to B0 follows each number. "
           "Ratios are descriptive; no statistical significance is implied. Hover a cell for "
           "its spread or validation errors." if suite == "S" and any(s.is_baseline for s in sets) else "")
        + "</p>")
    messages = [f"{s.label}: {p}" for s in sets for p in s.problems]
    base = next((s for s in sets if s.is_baseline), None)
    if base:
        messages += [f"{s.label}: comparison withheld: {p}" for s in sets if s is not base
                     for p in compatible(base.path, s.path)]
    if messages:
        parts.append("<ul>" + "".join(f"<li>{esc(m)}</li>" for m in messages) + "</ul>")
    parts.append("<nav>" + "".join(f"<a href='#{m}'>{esc(h)}</a>"
                                   for m, h, *_ in tables) + "</nav>")
    for metric, heading, unit, cols, rows in tables:
        one_size = _one_size(cols)
        parts.append(f"<h2 id='{metric}'>{esc(heading)} <span class='unit'>"
                     f"({esc(unit)})</span></h2><div class='wrap'><table><thead><tr>"
                     "<th>workload</th><th>variant</th>" +
                     ("" if one_size else "<th>size</th>") +
                     "<th class='num'>clients</th>" +
                     "".join(f"<th class='num'>{esc(s.label)}"
                             + (f"<br><span class='unit'>{esc(s.table_am)}</span>"
                                if s.label != s.table_am else "") + "</th>"
                             for s in cols) +
                     "</tr></thead><tbody>")
        prev = None
        for (w, variant, size, clients), cells in rows:
            first = w != prev
            prev = w
            parts.append(f"<tr class='{'first' if first else ''}'>"
                         f"<td class='wl'>{esc(_workload_title(w)) if first else ''}</td>"
                         f"<td>{esc(variant or '-')}</td>"
                         + ("" if one_size else f"<td>{esc(size)}</td>") +
                         f"<td class='num'>{clients}</td>")
            for c in cells:
                cls = ["num"]
                if not c.ok:
                    cls.append("na")
                elif c.better is True:
                    cls.append("good")
                elif c.better is False:
                    cls.append("bad")
                ratio = f"<span class='ratio'>{c.ratio:.2f}x</span>" \
                    if c.ratio is not None else ""
                parts.append(f"<td class='{' '.join(cls)}' title='{esc(c.detail)}'>"
                             f"{esc(c.text)}{ratio}</td>")
            parts.append("</tr>")
        parts.append("</tbody></table></div>")
    parts.append("</main></body></html>")
    return "".join(parts)
