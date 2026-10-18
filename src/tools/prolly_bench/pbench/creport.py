"""The C suite's pull-request comment, from every shard's result sets.

  prolly_bench creport SHARD_RESULTS... --shards N --base-baseline FILE --out comment.md

One comment per pull request, found and updated in place by its first
line, MARKER.  It has a headline (corpus pin, coverage, what was merged
into what, records, shards, wall clock, PASS or FAIL), a row per engine
(pass rate, change against the base commit's heap baseline, counts by
class, gate), then collapsed details: new failures, fixed records, stale
exceptions, time per cell, the per-shard tables, and how to reproduce.

The report also gates the shards as a whole: every shard 1..N present
and complete, with heap and every engine, and their files together the
whole corpus once.  A shard's own job runs cgate and cbaseline check on
its part; this module reads their comparisons from the result sets and
checks the heap records against the baseline again, so the comment and
the verdict come from the same data.
"""

from __future__ import annotations

import gzip
import json
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

from pbench import cbaseline, ccompare, cshard
from pbench.table import ResultSet, load_set

MARKER = "<!-- c-suite -->"
MAX_CHARS = 65000
PER_CLASS = 25
QUERY_CHARS = 300
# Engine classes with a column of their own; the rest count as "other".
COLUMNS = ["agree", "postgres_semantics", "regression", "hidden_regression", "order_only",
           "crashed", "timeout"]
HEADINGS = ["agree", "pg semantics", "regression", "hidden", "order only", "crashed",
            "timeout", "other"]


@dataclass
class Engine:
    am: str
    label: str = ""
    records: int = 0
    ran: int = 0
    ok: int = 0
    failing: int = 0                    # ran, didn't pass
    counts: Counter = field(default_factory=Counter)
    gate_failures: int = 0
    stale: list[dict[str, Any]] = field(default_factory=list)
    diffs: list[dict[str, Any]] = field(default_factory=list)
    seconds: dict[tuple[str, str], float] = field(default_factory=dict)   # (shard, workload)


@dataclass
class Report:
    text: str
    passed: bool
    problems: list[str]
    summary: dict[str, Any]


def _shard_of(rs: ResultSet) -> tuple[str, dict]:
    cfg = json.loads((rs.path / "config.json").read_text())
    spec, _ = cshard.recorded(cfg)
    return spec or "1/1", cfg


def _pct(ok: int, ran: int) -> Optional[float]:
    return 100 * ok / ran if ran else None


def _short(text: str, n: int = QUERY_CHARS) -> str:
    text = " ".join(text.split())
    return text if len(text) <= n else text[:n - 1] + "…"


def _cmp_dir(base: ResultSet, eng: ResultSet) -> Path:
    return eng.path / "compare" / base.path.name


def _comparisons(base: ResultSet, eng: ResultSet) -> dict[str, dict[str, Any]]:
    """The shard job's comparison of eng with base, as cgate left it in the
    result set; computed again only if it isn't there."""
    d = _cmp_dir(base, eng)
    found = {p.parent.name: json.loads(p.read_text()) for p in sorted(d.glob("*/compare.json"))}
    return found or ccompare.compare(base, eng)


def _diffs(base: ResultSet, eng: ResultSet) -> list[dict[str, Any]]:
    out = []
    for p in sorted(_cmp_dir(base, eng).glob("*/diffs.jsonl.gz")):
        with gzip.open(p, "rt") as f:
            out += [json.loads(line) for line in f]
    return out


def _covered(rs: ResultSet) -> dict[str, list[str]]:
    """Workload -> the files the set's records cover."""
    out = {}
    for w, d in ccompare.slt_dirs(rs).items():
        out[w] = sorted(json.loads((d / "corpus.json").read_text()))
    return out


def build(dirs: list[Path], shards: int, base_baseline: Path, *,
          baseline: Path = cbaseline.BASELINE, pin_file: Path = cbaseline.PIN,
          corpus: Optional[Path] = None, head: str = "", base: str = "",
          base_ref: str = "master", run_url: str = "", wall_clock: str = "",
          failed_jobs: list[str] = (), max_chars: int = MAX_CHARS) -> Report:
    problems: list[str] = [f"job failed: {j}" for j in failed_jobs]
    sets = []
    for d in dirs:
        try:
            sets.append(load_set(d))
        except (OSError, ValueError, KeyError) as e:
            problems.append(f"{d.name}: unreadable result set: {e}")
    commit = cbaseline.pin(pin_file)

    # Shard -> engine -> result set.
    by_shard: dict[str, dict[str, ResultSet]] = defaultdict(dict)
    cfgs: dict[str, dict] = {}
    for rs in sets:
        spec, cfg = _shard_of(rs)
        if rs.table_am in by_shard[spec]:
            problems.append(f"shard {spec}: two {rs.table_am} result sets")
        by_shard[spec][rs.table_am] = rs
        cfgs[spec] = cfg
        problems += [f"shard {spec} {rs.label}: {p}" for p in rs.problems]
    wanted = [f"{i}/{shards}" for i in range(1, shards + 1)]
    for spec in sorted(set(by_shard) - set(wanted)):
        problems.append(f"shard {spec}: not one of the {shards} shards expected")
    ams = sorted({am for e in by_shard.values() for am in e}, key=lambda am: am != "heap")
    engines = [am for am in ams if am != "heap"]
    if not engines:
        problems.append("no engine besides heap")
    for spec in wanted:
        got = by_shard.get(spec, {})
        if not got:
            problems.append(f"shard {spec}: missing")
        for am in ["heap"] + engines:
            if got and am not in got:
                problems.append(f"shard {spec}: no {am} result set")

    # Coverage: every shard's files, together the corpus once.
    heap_sets = [by_shard[s]["heap"] for s in wanted if "heap" in by_shard.get(s, {})]
    sample = "full corpus"
    if cfgs:
        cfg = next(iter(cfgs.values()))
        if any(p.get("sample", "all") != "all" for p in cfg.get("workloads", {}).values()):
            sample = "smoke"
        corpus = corpus or ccompare.corpus_path(heap_sets[0] if heap_sets else sets[0])
        try:
            full = cshard.workload_files(corpus, cfg)
            for am in ["heap"] + engines:
                parts = [_covered(by_shard[s][am]) for s in wanted if am in by_shard.get(s, {})]
                problems += [f"{am}: {p}" for p in cshard.overlap_and_gaps(parts, full)]
        except (OSError, ValueError) as e:
            problems.append(f"cannot list the corpus at {corpus}: {e}")

    # Heap against the baseline at the head, and the base's for the change.
    check = cbaseline.check(heap_sets, cbaseline.read(baseline), commit) if heap_sets else None
    if check and not check.ok:
        problems.append(check.summary())
    try:
        base_entries = cbaseline.read(base_baseline).entries
    except (OSError, ValueError, EOFError) as e:
        problems.append(f"base baseline {base_baseline}: {e}")
        base_entries = {}
    covered = check.files if check else set()
    base_failing = sum(1 for (f, _) in base_entries if f in covered)

    rows: dict[str, Engine] = {am: Engine(am) for am in ["heap"] + engines}
    for spec in wanted:
        got = by_shard.get(spec, {})
        for am, rs in got.items():
            e = rows.setdefault(am, Engine(am))
            e.label = rs.label
            for r in rs.results:
                w = r["key"]["workload"]
                if r["status"] != "ok":
                    problems.append(f"shard {spec} {rs.label}/{w}: {r['status']}")
                    continue
                m = r["metrics"]
                ran = m["records"] - m["skipped"]
                e.records += m["records"]
                e.ran += ran
                e.ok += m["ok"]
                e.failing += ran - m["ok"]
                e.seconds[(spec, w)] = m.get("window_seconds") or 0
                if am == "heap":
                    for st in ("crashed", "timeout"):
                        e.counts[st] += m.get(st, 0)
                    e.counts["other"] += m.get("not_implemented", 0) + m.get("did_not_run", 0)
                    e.counts["postgres_semantics"] += m.get("not_ok", 0)
        heap = got.get("heap")
        for am in engines:
            if heap is None or am not in got:
                continue
            e, rs = rows[am], got[am]
            try:
                cmps = _comparisons(heap, rs)
            except (RuntimeError, OSError, ValueError, KeyError) as err:
                problems.append(f"shard {spec} {am}: no comparison with heap: {err}")
                continue
            gated = set(ccompare.run_workloads(rs))
            if set(cmps) != gated:
                problems.append(f"shard {spec} {am}: compared {sorted(cmps)}, ran {sorted(gated)}")
            for w, cm in cmps.items():
                for cls, n in cm["counts"].items():
                    e.counts[cls if cls in COLUMNS else "other"] += n
                e.gate_failures += cm["gate_failures"]
                e.stale += cm["stale_exceptions"]
            e.diffs += [{**d, "shard": spec} for d in _diffs(heap, rs)
                        if d["class"] not in ("agree", "postgres_semantics", "exception")]
        for am in engines:
            if am in got and heap is not None:
                gated = sorted(set(ccompare.run_workloads(got[am])))
                if gated and sorted(ccompare.slt_dirs(got[am])) != gated:
                    problems.append(f"shard {spec} {am}: incomplete cells")
    for am in engines:
        e = rows[am]
        if e.gate_failures or e.diffs:
            problems.append(f"{am}: {max(e.gate_failures, len(e.diffs)):,} records differ from heap")
        if e.stale:
            problems.append(f"{am}: {len(e.stale)} stale exceptions")

    passed = not problems
    text = _render(rows, engines, check, base_failing, problems, passed, commit=commit,
                   sample=sample, shards=shards, head=head, base=base, base_ref=base_ref,
                   run_url=run_url, wall_clock=wall_clock, corpus=corpus,
                   by_shard=by_shard, wanted=wanted)
    text = truncate(text, max_chars, run_url)
    summary = {"pass": passed, "problems": problems, "shards": shards, "coverage": sample,
               "corpus": commit, "engines": {
                   am: {"records": e.ran, "ok": e.ok, "failing": e.failing,
                        "vs_base": e.failing - base_failing, "counts": dict(e.counts),
                        "differences": len(e.diffs), "stale_exceptions": len(e.stale)}
                   for am, e in rows.items()},
               "baseline": check.to_json() if check else None}
    return Report(text, passed, problems, summary)


def _vs_base(e: Engine, base_failing: int) -> str:
    delta = e.failing - base_failing
    if not delta:
        return "±0"
    pp = -100 * delta / e.ran if e.ran else 0
    return f"{pp:+.3f} pp ({delta:+,} failing)"


def _render(rows, engines, check, base_failing, problems, passed, *, commit, sample, shards,
            head, base, base_ref, run_url, wall_clock, corpus, by_shard, wanted) -> str:
    heap = rows["heap"]
    out = [MARKER,
           f"## C suite: sqllogictest @ {commit[:7]}, {sample}: {'PASS' if passed else 'FAIL'}",
           ""]
    what = (f"Merge of {head[:10]} into {base_ref} {base[:10]}" if head
            else f"{base_ref} {base[:10]}".strip())
    parts = [p for p in (what, f"{heap.ran:,} records",
                         f"{shards} shard{'s' if shards != 1 else ''}", wall_clock) if p]
    out += [" · ".join(parts), ""]
    out.append("| | Pass (corpus) | vs base | " + " | ".join(HEADINGS) + " | gate |")
    out.append("|---|---|---|" + "---|" * len(HEADINGS) + "---|")
    for am, e in rows.items():
        pct = _pct(e.ok, e.ran)
        cells = [f"{pct:.3f}%" if pct is not None else "-", _vs_base(e, base_failing)]
        if am == "heap":
            cells += ["—", f"{e.counts['postgres_semantics']:,}", "—", "—", "—",
                      f"{e.counts['crashed']:,}", f"{e.counts['timeout']:,}",
                      f"{e.counts['other']:,}"]
            gate = ("baseline: missing" if check is None else
                    f"baseline: {len(check.new) + len(check.changed):,} new, "
                    f"{len(check.fixed):,} fixed"
                    + (f", {len(check.incomplete):,} incomplete" if check.incomplete else "")
                    + ("" if check.ok else " (FAIL)"))
        else:
            cells += [f"{e.counts[c]:,}" for c in COLUMNS] + [f"{e.counts['other']:,}"]
            ok = not (e.gate_failures or e.diffs or e.stale) and e.ran
            gate = "pass" if ok else "FAIL"
        out.append(f"| {e.label or am} | " + " | ".join(cells) + f" | {gate} |")
    out.append("")
    if problems:
        out.append("**Problems**\n")
        out += [f"- {p}" for p in problems[:40]]
        if len(problems) > 40:
            out.append(f"- ... {len(problems) - 40} more")
        out.append("")

    # New failures, per class: the engines' differences, and heap's.
    groups: dict[str, list[tuple[str, dict]]] = defaultdict(list)
    for am in engines:
        for d in rows[am].diffs:
            groups[d["class"]].append((am, d))
    if check:
        for d in check.new + check.changed:
            groups["heap regression"].append(("heap", d))
    n = sum(len(v) for v in groups.values())
    out.append(f"<details><summary>New failures ({n:,})</summary>\n")
    if not n:
        out.append("None.\n")
    order = [c for c in ccompare.CLASSES if c in groups] + \
        sorted(c for c in groups if c not in ccompare.CLASSES)
    for cls in order:
        items = groups[cls]
        out.append(f"#### {cls}: {len(items):,}\n")
        for am, d in items[:PER_CLASS]:
            out.append(f"- `{d['file']}:{d['line']}` ({am}"
                       + (f", shard {d['shard']}" if d.get("shard") else "") + ")")
            sql = ccompare.query_text(corpus, d["file"], d["line"]) if corpus else "?"
            out.append(f"  - query: `{_short(sql)}`")
            if am == "heap":
                listed = " ".join(d["listed"]) if d.get("listed") else "passes"
                out.append(f"  - baseline: {listed}")
                out.append(f"  - heap: {' '.join(d['now'])}"
                           + (f", {_short(d['msg'], 160)}" if d.get("msg") else ""))
            else:
                out.append(f"  - heap: {ccompare._side(d.get('baseline'))}")
                out.append(f"  - {am}: {ccompare._side(d.get('engine'))}")
        if len(items) > PER_CLASS:
            out.append(f"- ... {len(items) - PER_CLASS:,} more")
        out.append("")
    out.append("</details>\n")

    fixed = check.fixed if check else []
    out.append(f"<details><summary>Fixed ({len(fixed):,}): baseline entries to remove"
               "</summary>\n")
    if fixed:
        out += [f"- `{d['file']}:{d['line']}` (was {' '.join(d['listed'])})" for d in fixed[:200]]
        if len(fixed) > 200:
            out.append(f"- ... {len(fixed) - 200:,} more")
        out.append("\n`prolly_bench cbaseline update` on a full heap run rewrites the file.")
    else:
        out.append("None.")
    out.append("\n</details>\n")

    stale = [(am, x) for am in engines for x in rows[am].stale]
    out.append(f"<details><summary>Stale exceptions ({len(stale)})</summary>\n")
    out += [f"- `{x['file']}:{x['line']}` ({am}): {x.get('reason', '')}" for am, x in stale] \
        or ["None."]
    out.append("\n</details>\n")

    out.append("<details><summary>Time per cell</summary>\n")
    out.append("| shard | workload | " + " | ".join(rows[am].label or am for am in rows) + " |")
    out.append("|---|---|" + "---|" * len(rows))
    cells = sorted({k for e in rows.values() for k in e.seconds},
                   key=lambda k: (cshard.parse(k[0])[0], int(k[1][1:])))
    for spec, w in cells:
        out.append(f"| {spec} | {w} | " + " | ".join(
            f"{rows[am].seconds[(spec, w)]:.0f} s" if (spec, w) in rows[am].seconds else "-"
            for am in rows) + " |")
    out.append("\n</details>\n")

    out.append("<details><summary>Per shard</summary>\n")
    for spec in wanted:
        got = by_shard.get(spec, {})
        if len(got) < 2:
            continue
        try:
            table = ccompare.to_markdown(list(got.values()))
        except (RuntimeError, OSError, ValueError, KeyError) as e:
            table = f"(no table: {e})"
        out.append(f"#### Shard {spec}\n")
        # (the headline and the table above already say what these lines do)
        out += [line.replace("## ", "##### ", 1) if line.startswith("## ") else line
                for line in table.splitlines()
                if not line.startswith(("# C suite", "Pass rate against", "The gate fails"))]
    out.append("\n</details>\n")

    out.append("Reproduce: `prolly_bench run C --am heap,prolly --shard I/N --config CONFIG`, "
               "then `prolly_bench cdiff HEAP_DIR ENGINE_DIR` lists every difference with its "
               "query" + (f" · [artifacts]({run_url})" if run_url else "") + ".")
    return "\n".join(out) + "\n"


def truncate(text: str, max_chars: int = MAX_CHARS, run_url: str = "") -> str:
    """text cut at a line to at most max_chars, its open <details> closed,
    with a notice."""
    if len(text) <= max_chars:
        return text
    notice = (f"\n\n**Truncated at {max_chars:,} characters**: the whole report is in the "
              + (f"[artifacts]({run_url})" if run_url else "artifacts") + ".\n")
    room = max_chars - len(notice) - len("\n</details>") * 8
    kept = text[:max(room, 0)]
    kept = kept[:kept.rfind("\n")] if "\n" in kept else kept
    opened = kept.count("<details>") - kept.count("</details>")
    return kept + "\n</details>" * max(opened, 0) + notice
