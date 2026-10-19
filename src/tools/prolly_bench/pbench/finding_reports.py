"""Full artifact reports from existing gate/calibration findings.

These adapters display decisions already made by the analyzers. Sticky
comments keep their separate bounded renderers and stable markers.
"""
from __future__ import annotations

import json

from pbench.presentation import Document, Section, Value


def _text(value) -> str:
    if value is None:
        return "—"
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, (dict, list)):
        return json.dumps(value, sort_keys=True, ensure_ascii=False)
    if isinstance(value, int):
        return f"{value:,}"
    if isinstance(value, float):
        return f"{value:,.6g}"
    return str(value)


def _records(document: Document, title: str, data, *, folded=False) -> None:
    """Expose every retained finding without imposing comment size limits."""
    if isinstance(data, dict):
        fields = [[str(k), _text(v)] for k, v in data.items() if not isinstance(v, (dict, list))]
        if fields:
            document.sections.append(Section(title, ["Field", "Value"], fields, folded=folded))
        for key, value in data.items():
            if isinstance(value, (dict, list)):
                _records(document, f"{title}: {key}", value, folded=folded)
    elif isinstance(data, list) and data:
        if all(isinstance(row, dict) for row in data):
            columns = list(dict.fromkeys(k for row in data for k in row))
            document.sections.append(Section(title, columns,
                [[_text(row.get(k)) for k in columns] for row in data], folded=folded))
        else:
            document.sections.append(Section(title, ["Finding"], [[_text(v)] for v in data], folded=folded))


def storage(counters: dict | None, timing: dict | None, *, passed: bool,
            metadata: dict, failed_jobs: list[str]) -> Document:
    from pbench import sreport
    document = Document("S suite: " + ("PASS" if passed else "FAIL"),
                        ["Full benchmark gate report. Verdicts and thresholds come from the existing S gate; "
                         "this artifact is not subject to GitHub comment size limits."], passed=passed)
    _records(document, "Provenance", metadata)
    if failed_jobs:
        _records(document, "Failed or missing jobs", failed_jobs)
    section = Section("Counter findings", ["Workload", "Engine", "Counter", "Completed unit", "Base", "Candidate",
                                           "Change", "Total change", "Result", "Reason"])
    for finding in (counters or {}).get("findings", []):
        f = finding
        status = f.get("result", "unknown")
        tone = "bad" if status in {"regression", "unstable_candidate", "invariant"} else "good" if status == "improvement" else ""
        section.rows.append([f.get("workload", "?"), f.get("engine", "?"), f.get("counter", "?"),
                             f.get("unit") or "transaction",
                             sreport._per(f, "base") if "base" in f else "—",
                             sreport._per(f, "candidate") if "candidate" in f else "—",
                             sreport._change(f) if "base" in f and "candidate" in f else "—",
                             sreport._total(f) if "base" in f and "candidate" in f else "—",
                             Value(status, tone=tone), f.get("reason") or f.get("detail", "")])
    section.notes.append("Base and candidate values are per completed unit when a unit denominator is supplied. "
                         "Total change covers the whole fixed-work run.")
    document.sections.append(section)
    if counters is None and not metadata.get("nightly"):
        document.notes.append("No counter findings were available.")
    section = Section("Timing findings", ["Workload", "Variant", "Size", "Clients", "Engine", "Metric", "Candidate/base ratio",
                                          "95% interval", "Result", "Attempts", "Problems"])
    for c in (timing or {}).get("cells", []):
        section.rows.append([c.get("workload", "?"), c.get("variant", "-"), c.get("size", "?"),
                             str(c.get("clients", "?")), c.get("engine", "?"), c.get("metric", "?"),
                             _text(c.get("ratio")), f"{_text(c.get('low'))} – {_text(c.get('high'))}",
                             c.get("result", "unknown"), _text(c.get("attempts", [])), _text(c.get("problems", []))])
    section.notes.append("Median paired ratios and existing retry/confirmation decisions. "
                         + ("Timing is report-only; it is not gated." if (timing or {}).get("report_only") else "Timing gate policy is recorded below."))
    document.sections.append(section)
    _records(document, "Counter evidence", counters or {}, folded=True)
    _records(document, "Timing evidence", timing or {}, folded=True)
    return document


def correctness(summary: dict, metadata: dict, details: dict | None = None) -> Document:
    passed = bool(summary.get("pass"))
    document = Document("C suite: " + ("PASS" if passed else "FAIL"),
                        [f"Corpus: {summary.get('corpus', '?')}; coverage: {summary.get('coverage', '?')}; "
                         f"expected shards: {summary.get('shards', '?')}. "
                         "Full correctness findings; the verdict is the existing cross-shard gate's."], passed=passed)
    _records(document, "Provenance", metadata)
    _records(document, "Problems", summary.get("problems", []))
    _records(document, "Notes", summary.get("notes", []))
    engines = summary.get("engines", {})
    classes = sorted({k for row in engines.values() for k in row.get("counts", {})})
    section = Section("Engine summary", ["Engine", "Records run", "Pass rate", "Failing", "Incomplete", "vs base",
                                         *classes, "Gate"])
    for engine, row in engines.items():
        rate = f"{100 * row['ok'] / row['ran']:.3f}%" if row.get("ran") else "—"
        section.rows.append([engine, _text(row.get("ran")), rate, _text(row.get("failing")),
                             _text(row.get("incomplete")), _text(row.get("vs_base")),
                             *[_text(row.get("counts", {}).get(k, 0)) for k in classes],
                             Value("PASS" if row.get("gate") else "FAIL", tone="good" if row.get("gate") else "bad")])
    document.sections.append(section)
    if details:
        _records(document, "Details", details)
        document.notes.append("Failure examples are sampled by the existing correctness analyzer; "
                              "complete per-record differences remain in the linked source artifacts.")
    _records(document, "Correctness evidence", summary, folded=True)
    return document


def calibration(data: dict) -> Document:
    document = Document("S suite calibration (A/A)",
                        ["Counter stability, timing noise and suggested gate settings from the existing calibration analysis."],
                        passed=bool(data.get("counters") or data.get("timing")))
    if not document.passed:
        document.notes.append("No valid calibration runs were available.")
    _records(document, "Suggested gate settings", data.get("suggested", {}))
    _records(document, "Excluded evidence", data.get("left_out", []))
    _records(document, "Counters", data.get("counters") or {})
    _records(document, "Timing", data.get("timing") or {})
    return document
