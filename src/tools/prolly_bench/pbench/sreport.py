"""The S suite's pull-request comment, from sgate's findings.

  prolly_bench sreport --counters counters.json [--timing timing.json] \\
      --head SHA --base SHA [--run-url URL] [--failed-job "NAME: RESULT"] \\
      --out comment.md

The comment starts with the marker <!-- s-suite --> (the comment workflow
finds and updates it by that), then the verdict, the counters that failed
or were accepted, the timing (reported), and in collapsed sections the
unstable counters, transient cells, improvements and the environment.  It
is at most 65,000 characters.  Exits 1 when the gate fails: a counter
regression without an accepted entry, an invariant, a missing job, or
(not report-only) a confirmed timing regression.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Optional

MARKER = "<!-- s-suite -->"
LIMIT = 65000

_UNIT = {"transaction": "txn", "row": "row", "query": "query"}


def _num(x: Optional[float]) -> str:
    if x is None:
        return "-"
    if abs(x) >= 100 or x == int(x):
        return f"{x:,.0f}"
    return f"{x:.3g}"


def _bytes(x: float) -> str:
    for unit, size in (("GB", 1 << 30), ("MB", 1 << 20), ("KB", 1 << 10)):
        if abs(x) >= size:
            return f"{x / size:+.2f} {unit}"
    return f"{x:+,.0f} B"


def _total(f: dict) -> str:
    if f["base"] is None or f["candidate"] is None:
        return "-"
    d = f["candidate"] - f["base"]
    from pbench.scounters import kind
    return _bytes(d) if kind(f["counter"]) == "bytes" else f"{d:+,.0f}"


def _per(f: dict, key: str) -> str:
    v, n = f[key], f.get("units")
    return _num(v / n) if v is not None and n else _num(v)


def _change(f: dict) -> str:
    b, c = f["base"], f["candidate"]
    if b is None or c is None:
        return "-"
    if b == 0:
        return "new" if c else "0%"
    return f"{(c - b) / b:+.1%}"


def _counter_rows(findings: list[dict]) -> list[str]:
    out = ["| Workload | Engine | Counter | Base | Candidate | Δ | Total Δ | Result |",
           "|---|---|---|---:|---:|---:|---:|---|"]
    for f in findings:
        result = {"regression": "**FAIL**", "unstable_candidate": "**FAIL** (unstable)",
                  "invariant": "**FAIL**", "improvement": "improved",
                  "unstable_base": "unstable in base"}.get(f["result"], f["result"])
        if f["result"] == "accepted":
            result = f"accepted: \"{f['reason']}\""
        if f["result"] == "invariant":
            out.append(f"| {f['workload']} | {f['engine']} | {f['counter']} | | | | | "
                       f"{result}: {f['detail']} |")
            continue
        unit = _UNIT.get(f.get("unit") or "transaction", f.get("unit"))
        out.append(f"| {f['workload']} | {f['engine']} | {f['counter']} (per {unit}) | "
                   f"{_per(f, 'base')} | {_per(f, 'candidate')} | {_change(f)} | {_total(f)} | {result} |")
    return out


def _ratio(c: dict) -> str:
    if c["ratio"] is None:
        return "-"
    if c["low"] is None:
        return f"{c['ratio']:.2f}"
    return f"{c['ratio']:.2f} [{c['low']:.2f}, {c['high']:.2f}]"


def _timing_rows(cells: list[dict]) -> list[str]:
    by = {}
    for c in cells:
        by.setdefault((c["workload"], c["clients"], c["engine"]), {})[c["metric"]] = c
    out = ["| Workload | Clients | Engine | TPS ratio | p50 ratio | prolly/heap base → candidate | Result |",
           "|---|---:|---|---:|---:|---|---|"]
    order = {"confirmed": 0, "accepted": 1, "transient": 2, "inconclusive": 3,
             "regressed": 0, "improved": 4, "same": 5, "no interval": 6}
    for (w, clients, engine), m in by.items():
        tps, p50 = m.get("throughput"), m.get("latency_p50_ms")
        heap = (tps or p50 or {}).get("over_heap") or {}
        heap_text = " → ".join("-" if v is None else f"{v:.2f}" for v in heap.values())
        results = sorted({c["result"] for c in m.values()}, key=lambda r: order.get(r, 9))
        out.append(f"| {w} | {clients} | {engine} | {_ratio(tps) if tps else '-'} | "
                   f"{_ratio(p50) if p50 else '-'} | {heap_text} | {', '.join(results)} |")
    return out


def render(counters: Optional[dict], timing: Optional[dict], *, head: str, base: str,
           runner: str, run_url: str = "", failed_jobs: list[str] = (),
           reproduce: str = "") -> tuple[str, bool]:
    """(comment, passed)."""
    findings = counters["findings"] if counters else []
    failed = [f for f in findings if f["result"] in ("regression", "unstable_candidate", "invariant")]
    accepted = [f for f in findings if f["result"] == "accepted"]
    unstable = [f for f in findings if f["result"] == "unstable_base"]
    improved = [f for f in findings if f["result"] == "improvement"]
    cells = timing["cells"] if timing else []
    report_only = bool(timing and timing.get("report_only"))
    confirmed = [c for c in cells if c["result"] == "confirmed"]
    passed = (counters is not None and counters["passed"] and not failed_jobs
              and (report_only or not confirmed))

    verdict = "PASS" if passed else "FAIL"
    what = "counters" if not timing else ("counters; timing reported" if report_only
                                          else "counters and timing")
    lines = [MARKER, f"## S suite: {verdict} ({what})",
             f"Base `{base[:10]}` · candidate `{head[:10]}` · on {runner}", ""]
    for job in failed_jobs:
        lines.append(f"- **{job}**: the job didn't finish, so the gate fails")
    if counters is None:
        lines.append("- **No counter results**: the counter runs left none.")
    if failed_jobs or counters is None:
        lines.append("")

    t = (counters or {}).get("thresholds", {})
    lines.append(f"### Counters (a regression is > {t.get('relative', 0.05):.0%} and, "
                 "in total over the fixed work, over its floor)")
    if failed or accepted:
        lines += _counter_rows(failed + accepted)
    elif counters is not None:
        lines.append("No counter regressed.")
    lines.append("")

    if timing:
        thr = timing.get("thresholds", {}).get("timing_threshold", 0.05)
        lines.append(f"### Timing (median ratio, 95% CI; regression if the CI clears {thr:.0%})"
                     + (" — reported, not gated" if report_only else ""))
        lines += _timing_rows(cells) if cells else ["No timing results."]
        lines.append("")

    if accepted:
        lines.append("### Accepted regressions")
        lines += [f"- {f['workload']} {f['engine']} {f['counter']}: {f['reason']}" for f in accepted]
        lines.append("")

    def details(title: str, body: list[str]) -> None:
        if body:
            lines.extend([f"<details><summary>{title}</summary>", "", *body, "", "</details>", ""])

    details(f"Unstable counters in the base ({len(unstable)}; reported, not gated)",
            _counter_rows(unstable) if unstable else [])
    transient = [c for c in cells if c["result"] in ("transient", "confirmed")]
    details(f"Cells that regressed, and their attempts ({len(transient)})",
            [f"- {c['workload']} c{c['clients']} {c['engine']} {c['metric']}: {c['result']} "
             f"({', '.join(c['attempts'])})" for c in transient])
    details(f"Improvements ({len(improved)})", _counter_rows(improved) if improved else [])
    env = [f"- runner: {runner}", f"- base: `{base}`", f"- candidate: `{head}`"]
    if run_url:
        env.append(f"- artifacts: {run_url}")
    details("Environment", env)
    if reproduce:
        lines += ["Reproduce:", "```sh", reproduce, "```"]

    text = "\n".join(lines) + "\n"
    if len(text) > LIMIT:
        cut = "\n\n(cut at 65,000 characters: the run's artifacts have it all)\n"
        text = text[:LIMIT - len(cut)] + cut
    return text, passed


def _load(path: Optional[Path]) -> Optional[dict]:
    return json.loads(path.read_text()) if path and path.exists() else None


def merge_counters(parts: list[dict]) -> Optional[dict]:
    """One counters finding set from several (sgate counters per group of
    workloads)."""
    if not parts:
        return None
    return {"layer": "counters", "thresholds": parts[0]["thresholds"],
            "passed": all(p["passed"] for p in parts),
            "findings": [f for p in parts for f in p["findings"]],
            "accepted": parts[0].get("accepted", [])}


def run(args) -> int:
    parts = [json.loads(p.read_text()) for p in args.counters]
    text, passed = render(merge_counters(parts), _load(args.timing), head=args.head,
                          base=args.base, runner=args.runner, run_url=args.run_url or "",
                          failed_jobs=args.failed_job, reproduce=args.reproduce or "")
    args.out.write_text(text)
    print(text)
    return 0 if passed else 1
