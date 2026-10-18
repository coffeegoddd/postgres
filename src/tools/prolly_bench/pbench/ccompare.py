"""The C suite's comparison of engines with heap (B0).

  prolly_bench table --suite C             counts per engine, and each vs B0
  prolly_bench cdiff BASELINE_DIR DIR      one engine's differences in full

slt-runner's compare subcommand does the record-by-record work (classes in
slt/compare.go); this module runs it per workload, keeps its output in the
engine's result set under compare/<baseline>/<workload>/, and renders it.
"""

from __future__ import annotations

import datetime as dt
import functools
import gzip
import json
import re
import subprocess
import tempfile
import tomllib
from collections import Counter, defaultdict
from itertools import zip_longest
from pathlib import Path
from typing import Any, Iterator, Optional

from pbench.table import ResultSet
from pbench.workloads import c_suite
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
    return c_suite.corpus_dir(cfg["tools"])


def run_workloads(rs: ResultSet) -> list[str]:
    """The workloads cgate requires by default: a shard's own (those it has
    files of, cshard.py), else all of the suite's."""
    cfg = json.loads((rs.path / "config.json").read_text())
    if cfg.get("run", {}).get("shard"):
        return list(cfg["run"]["workloads"])
    return list(c_suite.WORKLOADS)


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
        dest = comparison_dir(base, eng) / w
        result = dest / "compare.json"
        if _stamp(base, eng, w) != _stamped(dest):
            with tempfile.NamedTemporaryFile("w", suffix=".json") as f:
                json.dump(exc, f)
                f.flush()
                r = subprocess.run([str(runner), "compare", str(base_dirs[w]),
                                    str(eng_dirs[w]), str(dest), f.name],
                                   capture_output=True, text=True, timeout=3600)
            if r.returncode != 0:
                raise RuntimeError(f"slt-runner compare failed: {r.stderr[-2000:]}")
            seal_comparison(base, eng, w)
        out[w] = json.loads(result.read_text())
    return out


def comparison_dir(base: ResultSet, eng: ResultSet) -> Path:
    return eng.path / "compare" / base.path.name


def _stamp(base: ResultSet, eng: ResultSet, w: str) -> dict[str, Any]:
    """What a comparison of workload w was computed from, by role rather
    than path, so a result set's comparison stays valid when it is copied
    (CI's gate job downloads the shards' result sets elsewhere)."""
    identity = {"baseline": sha256(slt_dirs(base)[w] / "records.jsonl.gz"),
                "engine": sha256(slt_dirs(eng)[w] / "records.jsonl.gz")}
    runner = runner_path(eng)
    if runner.is_file():
        identity["runner"] = sha256(runner)
    if EXCEPTIONS.exists():
        identity["exceptions"] = sha256(EXCEPTIONS)
    # (exceptions expire: a result is for the day it was computed)
    identity["evaluated_on"] = dt.date.today().isoformat()
    dest = comparison_dir(base, eng) / w
    return {"inputs": identity, "result": sha256(dest / "compare.json") if
            (dest / "compare.json").exists() else None,
            "diffs": sha256(dest / "diffs.jsonl.gz") if (dest / "diffs.jsonl.gz").exists()
            else None}


def _stamped(dest: Path) -> Any:
    try:
        return json.loads((dest / "inputs.json").read_text())
    except (OSError, ValueError):
        return None


def seal_comparison(base: ResultSet, eng: ResultSet, w: str) -> None:
    """Record what the comparison now in eng's compare directory for w was
    computed from."""
    (comparison_dir(base, eng) / w / "inputs.json").write_text(
        json.dumps(_stamp(base, eng, w), sort_keys=True))


def is_status_problem(problem: str) -> bool:
    """Whether a problem from evidence() is only a record that didn't run to
    a result (one statuses=False leaves out)."""
    return re.fullmatch(r"record \S+:\d+ \w+", problem) is not None


# The files evidence() reads, and where it keeps its result for them.
EVIDENCE_INPUTS = ("summary.json", "corpus.json", "planned.jsonl.gz", "records.jsonl.gz",
                   "files.jsonl")
EVIDENCE_CACHE = "evidence.json"
EVIDENCE_VERSION = 1


def evidence(d: Path, am: str, statuses: bool = True) -> tuple[list[str], dict]:
    """Reconcile raw records against the plan and summary; never trust counts alone.
    With statuses, a record that didn't run to a result (crashed, timed out,
    ...) is a problem too.

    The full corpus is millions of records, and loading a result set,
    cgate, cbaseline and creport each reconcile them; the result is kept
    in d/evidence.json, keyed by the inputs' hashes, so the files are read
    once until one of them changes."""
    try:
        inputs = {n: sha256(d / n) for n in EVIDENCE_INPUTS}
    except OSError:
        return _evidence(d, am, statuses)
    key = f"{am}/{'statuses' if statuses else 'records'}"
    cache = d / EVIDENCE_CACHE
    try:
        kept = json.loads(cache.read_text())
        if kept.get("version") != EVIDENCE_VERSION or kept.get("inputs") != inputs:
            kept = {}
    except (OSError, ValueError, AttributeError):
        kept = {}
    if key in kept.get("results", {}):
        found = kept["results"][key]
        return found["problems"], found["corpus"]
    problems, corpus = _evidence(d, am, statuses)
    results = {**kept.get("results", {}), key: {"problems": problems, "corpus": corpus}}
    try:
        cache.write_text(json.dumps({"version": EVIDENCE_VERSION, "inputs": inputs,
                                     "results": results}))
    except OSError:
        pass  # (a read-only copy: only slower)
    return problems, corpus


def _evidence(d: Path, am: str, statuses: bool) -> tuple[list[str], dict]:
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
                if statuses and status not in ("ok", "not_ok", "skipped") and len(problems) < 20:
                    problems.append(f"record {key[0]}:{key[1]} {status}")
                # (a client-side error has only its message: a result, not
                # missing evidence)
                if status in ("ok", "not_ok") and not (
                        r.get("sqlstate") or (r.get("o") and r.get("s"))
                        or (status == "not_ok" and r.get("msg"))):
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
    yield from compared_diffs(base, eng)


def compared_diffs(base: ResultSet, eng: ResultSet) -> Iterator[dict[str, Any]]:
    """The differences of the comparison compare() last left, streamed."""
    for d in sorted(comparison_dir(base, eng).glob("*/diffs.jsonl.gz")):
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

@functools.lru_cache(maxsize=8)
def _test_lines(path: Path) -> Optional[tuple[str, ...]]:
    try:
        return tuple(path.read_text(errors="replace").splitlines())
    except OSError:
        return None


def query_text(corpus: Path, file: str, line: int) -> str:
    """The SQL of the record whose SQL starts at line of file."""
    lines = _test_lines(corpus / file)
    if lines is None:
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
