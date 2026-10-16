"""The C suite's comparison of engines with heap (B0).

  prolly_bench table --suite C             counts per engine, and each vs B0
  prolly_bench cdiff BASELINE_DIR DIR      one engine's differences in full

slt-runner's compare subcommand does the record-by-record work (classes in
slt/compare.go); this module runs it per workload, keeps its output in the
engine's result set under compare/<baseline>/<workload>/, and renders it.
"""

from __future__ import annotations

import gzip
import json
import subprocess
import tempfile
import tomllib
from collections import Counter, defaultdict
from itertools import zip_longest
from pathlib import Path
from typing import Any, Iterator, Optional

from pbench.table import ResultSet
from pbench.core.validation import sha256, compatible

HERE = Path(__file__).resolve().parents[1]
EXCEPTIONS = HERE / "configs" / "c_exceptions.toml"

# Classes in display order; the gate fails on GATE_CLASSES.
CLASSES = ["agree", "order_only", "not_implemented", "regression", "hidden_regression",
           "postgres_semantics", "engine_only_pass", "crashed", "timeout", "did_not_run",
           "baseline_did_not_run", "exception", "unmatched"]
GATE_CLASSES = [c for c in CLASSES if c not in ("agree", "postgres_semantics", "exception")]
# Engine statuses in display order (slt/merge.go).
STATUSES = ["ok", "not_ok", "not_implemented", "crashed", "timeout", "did_not_run"]


def slt_dirs(rs: ResultSet) -> dict[str, Path]:
    """Workload -> slt-runner output directory, for cells that completed."""
    out = {}
    for r in rs.results:
        d = rs.path / r["raw_dir"] / "slt"
        if r["status"] == "ok" and (d / "records.jsonl.gz").exists():
            out[r["key"]["workload"]] = d
    return out


def runner_path(rs: ResultSet) -> Path:
    cfg = json.loads((rs.path / "config.json").read_text())
    return Path(cfg["tools"]["slt_runner"])


def corpus_path(rs: ResultSet) -> Path:
    cfg = json.loads((rs.path / "config.json").read_text())
    return Path(cfg["tools"]["sqllogictest"]).expanduser() / "test"


def exceptions_for(am: str, path: Path = EXCEPTIONS) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    with open(path, "rb") as f:
        entries = tomllib.load(f).get("exception", [])
    if any(not e.get("am") for e in entries):
        raise ValueError("correctness exceptions must name one engine")
    return [{k: v for k, v in e.items() if k != "am"}
            for e in entries if e["am"] == am]


def compare(base: ResultSet, eng: ResultSet) -> dict[str, dict[str, Any]]:
    """Workload -> compare.json of eng against base, computed if missing or
    older than its inputs."""
    runner = runner_path(eng)
    base_dirs, eng_dirs = slt_dirs(base), slt_dirs(eng)
    exc = exceptions_for(eng.table_am)
    out = {}
    for w in sorted(set(base_dirs) & set(eng_dirs)):
        dest = eng.path / "compare" / base.path.name / w
        result = dest / "compare.json"
        inputs = [base_dirs[w] / "records.jsonl.gz", eng_dirs[w] / "records.jsonl.gz"]
        if runner.is_file():
            inputs.append(runner)
        if EXCEPTIONS.exists():
            inputs.append(EXCEPTIONS)
        stamp = dest / "inputs.json"
        identity = {str(p): sha256(p) for p in inputs}
        differences = dest / "diffs.jsonl.gz"
        valid_cache = False
        if result.exists() and stamp.exists() and differences.exists():
            try:
                valid_cache = json.loads(stamp.read_text()) == {
                    "inputs": identity, "result": sha256(result), "diffs": sha256(differences)}
            except ValueError:
                pass
        if not valid_cache:
            with tempfile.NamedTemporaryFile("w", suffix=".json") as f:
                json.dump(exc, f)
                f.flush()
                r = subprocess.run([str(runner), "compare", str(base_dirs[w]),
                                    str(eng_dirs[w]), str(dest), f.name],
                                   capture_output=True, text=True, timeout=3600)
            if r.returncode != 0:
                raise RuntimeError(f"slt-runner compare failed: {r.stderr[-2000:]}")
            stamp.write_text(json.dumps({"inputs": identity, "result": sha256(result),
                                        "diffs": sha256(differences)}, sort_keys=True))
        out[w] = json.loads(result.read_text())
    return out


def evidence(d: Path, am: str) -> tuple[list[str], dict]:
    """Reconcile raw records against the plan and summary; never trust counts alone."""
    problems = []
    try:
        summary = json.loads((d / "summary.json").read_text())
        corpus = json.loads((d / "corpus.json").read_text())
        if summary["am"] != am or not corpus:
            problems.append("wrong engine or empty corpus")
        if summary["wrong_am_files"] or summary["unmatched_digests"] or summary["unparsed_lines"]:
            problems.append("wrong target, unmatched digests or unparsed log records")
        counts, previous, total = Counter(), None, 0
        with gzip.open(d / "planned.jsonl.gz", "rt") as plan, gzip.open(d / "records.jsonl.gz", "rt") as records:
            for expected, actual in zip_longest(plan, records):
                if expected is None or actual is None:
                    raise ValueError("missing or unexpected records")
                expected, r = json.loads(expected), json.loads(actual)
                key = (r["file"], r["line"])
                status = r["status"]
                if (key != (expected["file"], expected["line"]) or r["file"] not in corpus
                        or previous is not None and key <= previous or r.get("version") != 2):
                    raise ValueError("duplicate, unordered, unplanned or legacy record")
                if expected["skip"] != (status == "skipped"):
                    raise ValueError("unexpected skip or execution of excluded record")
                if status not in ("ok", "not_ok", "skipped") and len(problems) < 20:
                    problems.append(f"{key}: {status}")
                if status in ("ok", "not_ok") and not (r.get("sqlstate") or (r.get("o") and r.get("s"))):
                    raise ValueError("missing actual execution evidence")
                previous = key
                total += 1
                counts[status] += 1
        if not total:
            problems.append("empty run")
        if summary["total"] != total or +Counter(summary["counts"]) != counts:
            problems.append("summary counts differ from raw records")
        files = [json.loads(line) for line in (d / "files.jsonl").read_text().splitlines()]
        if (summary["files"] != len(files) or len(files) != len(corpus)
                or {f["file"] for f in files} != corpus.keys()
                or any(f.get("wrong_am") or f.get("event") for f in files)):
            problems.append("incomplete or failed file evidence")
        return problems, corpus
    except (OSError, EOFError, ValueError, KeyError, TypeError) as e:
        return [f"invalid correctness evidence: {e}"], {}


def gate(base: ResultSet, eng: ResultSet, workloads: list[str]) -> list[str]:
    """Strict CI gate: every requested cell must run and have comparable answers.

    Order-only findings require review here; the reporting comparator doesn't
    distinguish a required ORDER BY from an unspecified result order.
    """
    problems = compatible(base.path, eng.path)
    if base.suite != "C" or eng.suite != "C" or base.table_am != "heap":
        return ["the gate needs C results with heap as the baseline"]
    required = set(workloads)
    if not required:
        return ["no required workloads"]
    for name, rs in (("heap", base), (eng.table_am, eng)):
        cells = [r["key"]["workload"] for r in rs.results]
        if set(cells) != required or len(cells) != len(required):
            problems.append(f"{name}: expected exactly one cell for {sorted(required)}, got {cells}")
        dirs = slt_dirs(rs)
        if set(dirs) != required:
            problems.append(f"{name}: incomplete cells or missing records")
        for w, d in dirs.items():
            errors, corpus = evidence(d, rs.table_am)
            problems.extend(f"{name}/{w}: {e}" for e in errors)
            if name != "heap" and w in slt_dirs(base):
                _, baseline_corpus = evidence(slt_dirs(base)[w], "heap")
                if corpus != baseline_corpus:
                    problems.append(f"{name}/{w}: different corpus contents")
    if problems:
        return problems
    try:
        comparisons = compare(base, eng)
    except (RuntimeError, OSError, ValueError, subprocess.TimeoutExpired) as e:
        return [f"comparison failed: {e}"]
    if set(comparisons) != required:
        problems.append("missing workload comparisons")
    for w, result in comparisons.items():
        counts = result["counts"]
        expected = json.loads((slt_dirs(eng)[w] / "summary.json").read_text())["total"]
        if sum(counts.values()) != expected or expected <= 0:
            problems.append(f"{w}: no compared records")
        for kind, count in counts.items():
            if count and kind not in ("agree", "postgres_semantics", "exception"):
                problems.append(f"{w}: {count} {kind}")
        if result["stale_exceptions"]:
            problems.append(f"{w}: stale exceptions")
    return problems


def diffs(base: ResultSet, eng: ResultSet) -> Iterator[dict[str, Any]]:
    compare(base, eng)
    for d in sorted((eng.path / "compare" / base.path.name).glob("*/diffs.jsonl.gz")):
        with gzip.open(d, "rt") as f:
            for line in f:
                yield json.loads(line)


# ---------------------------------------------------------------------------
# Tables
# ---------------------------------------------------------------------------

def _row_metrics(rs: ResultSet) -> dict[str, dict[str, Any]]:
    return {r["key"]["workload"]: r for r in rs.results}


def _pct(ok: int, ran: int) -> str:
    return f"{100 * ok / ran:.3f}%" if ran else "-"


def to_markdown(sets: list[ResultSet]) -> str:
    if len({s.label for s in sets}) != len(sets):
        raise ValueError("result labels must be unique")
    sets = sorted(sets, key=lambda s: (not s.is_baseline, s.label))
    base = next((s for s in sets if s.is_baseline), None)
    workloads = sorted({r["key"]["workload"] for s in sets for r in s.results},
                       key=lambda w: int(w[1:]))
    out = ["# C suite by engine\n",
           "Pass rate against the corpus's expected answers (records that ran), "
           "then each engine's records compared with heap's.  Columns: "
           + ", ".join(f"{s.label} = `{s.path.name}`" for s in sets) + ".\n",
           "## Pass rate\n",
           "| workload | " + " | ".join(s.label for s in sets) + " |",
           "|---|" + "---|" * len(sets)]
    totals = {s.label: Counter() for s in sets}
    for w in workloads + ["total"]:
        row = [w]
        for s in sets:
            if w == "total":
                t = totals[s.label]
                row.append(_pct(t["ok"], t["ran"]))
                continue
            r = _row_metrics(s).get(w)
            if r is None:
                row.append("-")
            elif r["status"] != "ok":
                row.append(r["status"])
            else:
                m = r["metrics"]
                ran = m["records"] - m["skipped"]
                totals[s.label].update(ok=m["ok"], ran=ran)
                row.append(_pct(m["ok"], ran))
        out.append("| " + " | ".join(row) + " |")
    out.append("")

    if base is None:
        out.append("No B0 (heap) result set, so no comparison.\n")
        return "\n".join(out)
    for s in sets:
        if s is base:
            continue
        problems = gate(base, s, workloads)
        try:
            cmps = compare(base, s) if not problems else {}
        except (RuntimeError, OSError, ValueError):
            cmps = {}
        out.append(f"## {s.label} compared with {base.label}\n")
        present = [c for c in CLASSES
                   if c == "regression" or any(cm["counts"].get(c) for cm in cmps.values())]
        out.append("| workload | " + " | ".join(present) + " | gate |")
        out.append("|---|" + "---|" * (len(present) + 1))
        total, failures, stale = Counter(), 0, []
        for w in workloads:
            cm = cmps.get(w)
            if cm is None:
                out.append(f"| {w} | " + " | ".join("-" for _ in present) + " | FAIL |")
                continue
            total.update(cm["counts"])
            failures += cm["gate_failures"]
            stale += cm["stale_exceptions"]
            out.append(f"| {w} | " + " | ".join(f"{cm['counts'].get(c, 0):,}" for c in present)
                       + f" | {'pass' if cm['gate_failures'] == 0 else 'FAIL'} |")
        out.append("| total | " + " | ".join(f"{total.get(c, 0):,}" for c in present)
                   + f" | {'pass' if not problems and total else 'FAIL'} |")
        out.extend(f"\n- {p}" for p in problems)
        out.append("")
        if stale:
            out.append("Stale exceptions (they no longer differ): "
                       + ", ".join(f"{e['file']}:{e['line']}" for e in stale) + "\n")
    out.append("The gate fails on " + ", ".join(GATE_CLASSES) + ".  `prolly_bench cdiff "
               "BASELINE_DIR DIR` lists the differences.\n")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# cdiff
# ---------------------------------------------------------------------------

def query_text(corpus: Path, file: str, line: int) -> str:
    """The SQL of the record whose SQL starts at line of file."""
    try:
        lines = (corpus / file).read_text(errors="replace").splitlines()
    except OSError:
        return "?"
    sql = []
    for text in lines[line - 1:]:
        if not text.strip() or text.startswith("----"):
            break
        sql.append(text)
    return "\n".join(sql)


def _side(r: Optional[dict]) -> str:
    if r is None:
        return "missing"
    parts = [r["status"]]
    if r.get("sqlstate"):
        parts.append(r["sqlstate"])
    if r.get("rows") or r.get("s"):
        parts.append(f"{r.get('rows', 0)} rows")
    if r.get("msg"):
        parts.append(r["msg"][:160])
    return ", ".join(parts)


def cdiff(base: ResultSet, eng: ResultSet, classes: Optional[list[str]] = None,
          limit: int = 20) -> str:
    """Differences grouped by class, then by the engine's error, each with
    examples: file and line, the query, both sides."""
    corpus = corpus_path(eng)
    groups: dict[str, dict[str, list[dict]]] = defaultdict(lambda: defaultdict(list))
    for d in diffs(base, eng):
        if classes and d["class"] not in classes:
            continue
        e = d.get("engine") or {}
        what = e.get("msg") or e.get("status", "")
        groups[d["class"]][what].append(d)
    out = [f"# {eng.label} compared with {base.label}\n",
           f"`{eng.path.name}` against `{base.path.name}`.\n"]
    for cls in [c for c in CLASSES if c in groups]:
        n = sum(len(v) for v in groups[cls].values())
        out.append(f"## {cls}: {n:,}\n")
        for what, ds in sorted(groups[cls].items(), key=lambda kv: -len(kv[1])):
            out.append(f"### {len(ds):,} × {what[:200] or '(no message)'}\n")
            files = Counter(d["file"] for d in ds)
            out.append("Files: " + ", ".join(f"{f} ({n})" for f, n in files.most_common(5))
                       + (", ..." if len(files) > 5 else "") + "\n")
            for d in ds[:limit]:
                out.append(f"- `{d['file']}:{d['line']}`"
                           + (f" (exception: {d['reason']})" if d.get("reason") else ""))
                out.append("  ```sql\n  " + query_text(corpus, d["file"], d["line"])
                           .replace("\n", "\n  ") + "\n  ```")
                out.append(f"  - {base.label}: {_side(d.get('baseline'))}")
                out.append(f"  - {eng.label}: {_side(d.get('engine'))}")
            if len(ds) > limit:
                out.append(f"- ... {len(ds) - limit:,} more")
            out.append("")
    if len(out) == 2:
        out.append("No differences.\n")
    return "\n".join(out)
