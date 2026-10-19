"""The S suite's pull-request comment, from sgate's findings.

  prolly_bench sreport --counters counters.json [--timing timing.json] \\
      --head SHA --base SHA [--run-url URL] [--failed-job "NAME: RESULT"] \\
      --out comment.md

The comment starts with the marker <!-- s-suite --> (the comment workflow
finds and updates it by that), then the verdict, the counters that failed
or were accepted, the timing (reported), and in collapsed sections the
unstable counters, jitter, retried cells, improvements and the environment.
It is at most 65,000 characters: collapsed sections that don't fit are left
out whole, never cut open.  Exits 1 when the gate fails: a counter
regression without an accepted entry, an invariant, a missing job or
results file, invalid timing evidence, or (not report-only) a confirmed
timing regression.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Optional

from pbench.scounters import kind

MARKER = "<!-- s-suite -->"
LIMIT = 65000

_UNIT = {"transaction": "txn", "row": "row", "query": "query"}


def _cell(text) -> str:
    """text for a Markdown table cell: its pipes escaped, on one line."""
    return " ".join(str(text).split()).replace("|", "\\|")


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
            result = f"accepted: \"{_cell(f['reason'])}\""
        lead = f"| {_cell(f['workload'])} | {_cell(f['engine'])} | {_cell(f['counter'])}"
        if f["result"] == "invariant":
            out.append(f"{lead} | | | | | {result}: {_cell(f['detail'])} |")
            continue
        unit = _UNIT.get(f.get("unit") or "transaction", f.get("unit"))
        out.append(f"{lead} (per {unit}) | {_per(f, 'base')} | {_per(f, 'candidate')} | "
                   f"{_change(f)} | {_total(f)} | {result} |")
    return out


def _ratio(c: Optional[dict]) -> str:
    if not c or c["ratio"] is None:
        return "-"
    if c["low"] is None:
        return f"{c['ratio']:.2f}"
    return f"{c['ratio']:.2f} [{c['low']:.2f}, {c['high']:.2f}]"


def _timing_rows(cells: list[dict]) -> list[str]:
    by = {}
    for c in cells:
        by.setdefault((c["workload"], c["clients"], c["engine"]), {})[c["metric"]] = c
    out = ["| Workload | Clients | Engine | TPS ratio | p50 ratio | p99 ratio | "
           "prolly/heap base → candidate | Result |",
           "|---|---:|---|---:|---:|---:|---|---|"]
    order = {"invalid": 0, "confirmed": 0, "accepted": 1, "transient": 2, "inconclusive": 3,
             "regressed": 0, "improved": 4, "same": 5, "no interval": 6}
    for (w, clients, engine), m in by.items():
        results = sorted({c["result"] for c in m.values()}, key=lambda r: order.get(r, 9))
        problems = [p for c in m.values() for p in c.get("problems", [])]
        if "invalid" in results:
            out.append(f"| {_cell(w)} | {clients or ''} | {_cell(engine)} | | | | | "
                       f"**invalid**: {_cell('; '.join(problems))} |")
            continue
        tps = m.get("throughput")
        heap = (tps or next(iter(m.values()))).get("over_heap") or {}
        heap_text = " → ".join("-" if v is None else f"{v:.2f}" for v in heap.values())
        out.append(f"| {_cell(w)} | {clients} | {_cell(engine)} | {_ratio(tps)} | "
                   f"{_ratio(m.get('latency_p50_ms'))} | {_ratio(m.get('latency_p99_ms'))} | "
                   f"{heap_text} | {', '.join(results)} |")
    return out


def _fit(main: list[str], sections: list[list[str]], tail: list[str]) -> str:
    """main, then the sections and tail that fit in LIMIT, whole; main cut
    at a line if it alone doesn't (it holds no open block)."""
    cut = "(cut at 65,000 characters: the run's artifacts have it all)"
    text = "\n".join(main) + "\n"
    if len(text) > LIMIT:
        keep = []
        size = len(cut) + 2
        for line in main:
            if size + len(line) + 1 > LIMIT:
                break
            keep.append(line)
            size += len(line) + 1
        return "\n".join(keep + ["", cut]) + "\n"
    dropped = False
    for block in sections + [tail]:
        piece = "\n".join(block) + "\n" if block else ""
        if len(text) + len(piece) + len(cut) + 2 > LIMIT:
            dropped = True
            continue
        text += piece
    return text + ("\n" + cut + "\n" if dropped else "")


def render(counters: Optional[dict], timing: Optional[dict], *, head: str, base: str,
           runner: str, run_url: str = "", failed_jobs: list[str] = (),
           reproduce: str = "", nightly: bool = False) -> tuple[str, bool]:
    """(comment, passed).  nightly: a night against the night before,
    timing only; it passes when its timing has results, valid, and no
    regression is confirmed."""
    findings = counters["findings"] if counters else []
    failed = [f for f in findings if f["result"] in ("regression", "unstable_candidate", "invariant")]
    accepted = [f for f in findings if f["result"] == "accepted"]
    unstable = [f for f in findings if f["result"] == "unstable_base"]
    jitter = [f for f in findings if f["result"] == "jitter"]
    improved = [f for f in findings if f["result"] == "improvement"]
    cells = timing["cells"] if timing else []
    report_only = bool(timing and timing.get("report_only"))
    confirmed = [c for c in cells if c["result"] == "confirmed"]
    timing_failed = [c for c in cells if c["result"] == "invalid"
                     or (c["result"] == "confirmed" and not report_only)]
    if nightly:
        compared = [c for c in cells if c["result"] != "invalid"]
        passed = bool(compared) and not confirmed and not timing_failed and not failed_jobs
        verdict = f"{len(confirmed)} confirmed regression(s)" if confirmed else \
            "no confirmed regression" if passed else "no valid results"
        main = [MARKER, f"## S suite nightly: {verdict}",
                f"The night before `{base[:10]}` · tonight `{head[:10]}` · on {runner} "
                "(timing with a shared machine's noise)", ""]
    else:
        passed = (counters is not None and counters["passed"] and not failed_jobs
                  and not timing_failed)
        verdict = "PASS" if passed else "FAIL"
        what = "counters" if not timing else ("counters; timing reported" if report_only
                                              else "counters and timing")
        main = [MARKER, f"## S suite: {verdict} ({what})",
                f"Base `{base[:10]}` · candidate `{head[:10]}` · on {runner}", ""]
    for job in failed_jobs:
        main.append(f"- **{_cell(job)}**: the gate fails")
    if counters is None and not nightly:
        main.append("- **No counter results**: the counter runs left none.")
    if failed_jobs or (counters is None and not nightly):
        main.append("")

    if not nightly:
        t = (counters or {}).get("thresholds", {})
        main.append(f"### Counters (a regression is > {t.get('relative', 0.05):.0%} and, "
                    "in total over the fixed work, over its floor)")
        if failed or accepted:
            main += _counter_rows(failed + accepted)
        elif counters is not None:
            main.append("No counter regressed.")
        main.append("")

    if timing:
        thr = timing.get("thresholds", {}).get("timing_threshold", 0.05)
        main.append(f"### Timing (median ratio, 95% CI; regression if the CI clears {thr:.0%})"
                    + (" — reported, not gated" if report_only else ""))
        main += _timing_rows(cells) if cells else ["No timing results."]
        main.append("")

    if accepted:
        main.append("### Accepted regressions")
        main += [f"- {_cell(f['workload'])} {f['engine']} {f['counter']}: {_cell(f['reason'])}"
                 for f in accepted]
        main.append("")

    def details(title: str, body: list[str]) -> list[str]:
        return [f"<details><summary>{title}</summary>", "", *body, "", "</details>", ""] \
            if body else []

    transient = [c for c in cells if c["result"] in ("transient", "confirmed", "accepted")]
    env = [f"- runner: {runner}", f"- base: `{base}`", f"- candidate: `{head}`"]
    if run_url:
        env.append(f"- artifacts: {run_url}")
    sections = [
        details(f"Unstable counters in the base ({len(unstable)}; reported, not gated)",
                _counter_rows(unstable) if unstable else []),
        details(f"Cells that regressed, and their attempts ({len(transient)})",
                [f"- {_cell(c['workload'])} c{c['clients']} {c['engine']} {c['metric']}: "
                 f"{c['result']} ({', '.join(c['attempts'])})" for c in transient]),
        details(f"Improvements ({len(improved)})", _counter_rows(improved) if improved else []),
        details("Environment", env),
        details(f"Counters that differ between a build's own runs by less than a change "
                f"({len(jitter)})", _counter_rows(jitter) if jitter else []),
    ]
    tail = ["Reproduce:", "```sh", reproduce, "```"] if reproduce else []
    return _fit(main, sections, tail), passed


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
    failed_jobs = list(args.failed_job)
    parts = []
    for p in args.counters:
        part = _load(p)
        if part is None:
            failed_jobs.append(f"counter results missing: {p}")
        else:
            parts.append(part)
    timing = _load(args.timing)
    if args.timing and timing is None:
        failed_jobs.append(f"timing results missing: {args.timing}")
    text, passed = render(merge_counters(parts), timing, head=args.head,
                          base=args.base, runner=args.runner, run_url=args.run_url or "",
                          failed_jobs=failed_jobs, reproduce=args.reproduce or "",
                          nightly=getattr(args, "nightly", False))
    args.out.write_text(text)
    from pbench import finding_reports, presentation
    from pbench.presentation import Section, link
    output = getattr(args, "report_output", None) or args.out.with_name(args.out.stem + "-full")
    document = finding_reports.storage(merge_counters(parts), timing, passed=passed,
        metadata={"base": args.base, "head": args.head, "runner": args.runner,
                  "run_url": args.run_url or "", "reproduce": args.reproduce or "",
                  "nightly": getattr(args, "nightly", False)}, failed_jobs=failed_jobs)
    evidence = Section("Source artifacts", ["Evidence", "Location"])
    for path in [*args.counters, *([args.timing] if args.timing else []), *getattr(args, "results", [])]:
        evidence.rows.append([path.name, link(path, output.parent, str(path))])
    document.sections.append(evidence)
    presentation.write(document, output)
    print(text)
    return 0 if passed else 1
