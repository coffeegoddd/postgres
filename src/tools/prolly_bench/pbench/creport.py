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
    ran: int = 0                        # records that ran (not skipped)
    ok: int = 0
    incomplete: int = 0                 # timed out or didn't run
    counts: Counter = field(default_factory=Counter)
    gate_failures: int = 0
    stale: list[dict[str, Any]] = field(default_factory=list)
    # Gating differences: a count per class, and the first PER_CLASS of each.
    differences: Counter = field(default_factory=Counter)
    examples: dict[str, list[dict[str, Any]]] = field(default_factory=lambda: defaultdict(list))
    seconds: dict[tuple[str, str], float] = field(default_factory=dict)   # (shard, workload)
    failed: bool = False                # a problem names this engine

    @property
    def failing(self) -> int:
        """Records that ran to a result and didn't pass (the baseline's sense)."""
        return self.ran - self.ok - self.incomplete


@dataclass
class Report:
    text: str
    passed: bool
    problems: list[str]
    summary: dict[str, Any]
    details: dict[str, Any] = field(default_factory=dict)


def _shard_of(rs: ResultSet) -> tuple[str, dict]:
    cfg = json.loads((rs.path / "config.json").read_text())
    spec, _ = cshard.recorded(cfg)
    return spec or "1/1", cfg


def _short(text: str, n: int = QUERY_CHARS) -> str:
    text = " ".join(text.split())
    return text if len(text) <= n else text[:n - 1] + "…"


def _covered(rs: ResultSet) -> dict[str, list[str]]:
    """Workload -> the files the set's records cover."""
    out = {}
    for w, d in ccompare.slt_dirs(rs).items():
        out[w] = sorted(json.loads((d / "corpus.json").read_text()))
    return out


def build(dirs: list[Path], shards: int, base_baseline: Path, *,
          baseline: Path = cbaseline.BASELINE, pin_file: Path = cbaseline.PIN,
          engines: Optional[list[str]] = None, corpus: Optional[Path] = None,
          head: str = "", base: str = "", base_ref: str = "master", run_url: str = "",
          wall_clock: str = "", failed_jobs: list[str] = (),
          max_chars: int = MAX_CHARS) -> Report:
    """The comment over every shard's result sets in dirs.  engines are the
    engines besides heap every shard must have (default: those found)."""
    problems: list[str] = [f"job failed: {j}" for j in failed_jobs]
    notes: list[str] = []          # shown, but not failing the report
    failed: set[str] = set()       # engines a problem names ("*": all)

    def fail(message: str, *ams: str) -> None:
        problems.append(message)
        failed.update(ams or ("*",))

    sets = []
    for d in dirs:
        try:
            sets.append(load_set(d))
        except (OSError, ValueError, KeyError) as e:
            fail(f"{d.name}: unreadable result set: {e}")
    try:
        commit = cbaseline.pin(pin_file)
    except OSError as e:
        fail(f"no corpus pin: {e}")
        commit = "?"

    # Shard -> engine -> result set.
    by_shard: dict[str, dict[str, ResultSet]] = defaultdict(dict)
    cfgs: dict[str, dict] = {}
    for rs in sets:
        try:
            spec, cfg = _shard_of(rs)
        except (OSError, ValueError) as e:
            fail(f"{rs.path.name}: no configuration: {e}", rs.table_am)
            continue
        if rs.table_am in by_shard[spec]:
            fail(f"shard {spec}: two {rs.table_am} result sets", rs.table_am)
        by_shard[spec][rs.table_am] = rs
        cfgs[spec] = cfg
        for p in rs.problems:
            fail(f"shard {spec} {rs.label}: {p}", rs.table_am)
    wanted = [f"{i}/{shards}" for i in range(1, shards + 1)]
    for spec in sorted(set(by_shard) - set(wanted)):
        fail(f"shard {spec}: not one of the {shards} shards expected")
    found = {am for e in by_shard.values() for am in e if am != "heap"}
    engines = sorted(engines if engines else found)
    for am in sorted(found - set(engines)):
        fail(f"{am}: an engine that wasn't expected", am)
    if not engines:
        fail("no engine besides heap")
    for spec in wanted:
        got = by_shard.get(spec, {})
        if not got:
            fail(f"shard {spec}: missing")
        for am in ["heap"] + engines:
            if got and am not in got:
                fail(f"shard {spec}: no {am} result set", am)

    # Coverage: every shard's files, together the corpus once.
    heap_sets = [by_shard[s]["heap"] for s in wanted if "heap" in by_shard.get(s, {})]
    sample = "full corpus"
    if cfgs:
        cfg = next(iter(cfgs.values()))
        if any(p.get("sample", "all") != "all" for p in cfg.get("workloads", {}).values()):
            sample = "smoke"
        try:
            corpus = corpus or ccompare.corpus_path(heap_sets[0] if heap_sets else sets[0])
            full = cshard.workload_files(corpus, cfg)
            for am in ["heap"] + engines:
                parts = [_covered(by_shard[s][am]) for s in wanted if am in by_shard.get(s, {})]
                for p in cshard.overlap_and_gaps(parts, full):
                    fail(f"{am}: {p}", am)
        except (OSError, ValueError, KeyError) as e:
            fail(f"cannot list the corpus at {corpus}: {e}")

    # Heap against the baseline at the head, and the base's for the change.
    check = None
    if heap_sets:
        try:
            check = cbaseline.check(heap_sets, cbaseline.read(baseline), commit)
            if not check.ok:
                fail(check.summary(), "heap")
        except (OSError, ValueError, EOFError) as e:
            fail(f"heap baseline {baseline}: {e}", "heap")
    base_failing: Optional[int] = None
    try:
        base_file = cbaseline.read(base_baseline)
        if base_file.commit != commit:
            notes.append(f"the base's baseline is for sqllogictest {base_file.commit[:7]}, "
                         f"not {commit[:7]}: no change against the base")
        elif check:
            base_failing = sum(1 for (f, _) in base_file.entries if f in check.files)
    except (OSError, ValueError, EOFError) as e:
        notes.append(f"no heap baseline at the base ({e}): no change against the base")

    rows: dict[str, Engine] = {am: Engine(am) for am in ["heap"] + engines}
    for spec in wanted:
        got = by_shard.get(spec, {})
        for am, rs in sorted(got.items()):
            e = rows.setdefault(am, Engine(am))
            e.label = rs.label
            for r in rs.results:
                w = r["key"]["workload"]
                if r["status"] != "ok":
                    fail(f"shard {spec} {rs.label}/{w}: {r['status']}", am)
                    continue
                try:
                    m = r["metrics"]
                    ran = m["records"] - m["skipped"]
                    e.ok += m["ok"]
                    e.incomplete += m.get("timeout", 0) + m.get("did_not_run", 0)
                except (KeyError, TypeError) as err:
                    fail(f"shard {spec} {rs.label}/{w}: no metric {err}", am)
                    continue
                e.ran += ran
                e.seconds[(spec, w)] = m.get("window_seconds") or 0
                if am == "heap":
                    for st in ("crashed", "timeout"):
                        e.counts[st] += m.get(st, 0)
                    e.counts["other"] += m.get("not_implemented", 0) + m.get("did_not_run", 0)
                    e.counts["postgres_semantics"] += m.get("not_ok", 0)
            if am == "heap":
                continue
            heap = got.get("heap")
            if heap is None:
                continue
            gated = sorted(set(ccompare.run_workloads(rs)))
            if sorted(ccompare.slt_dirs(rs)) != gated:
                fail(f"shard {spec} {am}: incomplete cells", am)
            try:
                cmps = ccompare.compare(heap, rs)
                if sorted(cmps) != gated:
                    fail(f"shard {spec} {am}: compared {sorted(cmps)}, ran {gated}", am)
                for w, cm in cmps.items():
                    for cls, n in cm["counts"].items():
                        e.counts[cls if cls in COLUMNS else "other"] += n
                    e.gate_failures += cm["gate_failures"]
                    e.stale += cm["stale_exceptions"]
                for d in ccompare.compared_diffs(heap, rs):
                    if d["class"] in ("agree", "postgres_semantics", "exception"):
                        continue
                    e.differences[d["class"]] += 1
                    if len(e.examples[d["class"]]) < PER_CLASS:
                        e.examples[d["class"]].append({**d, "shard": spec})
            except (RuntimeError, OSError, ValueError, KeyError) as err:
                fail(f"shard {spec} {am}: no comparison with heap: {err}", am)
    for am in engines:
        e = rows[am]
        n = max(e.gate_failures, sum(e.differences.values()))
        if n:
            fail(f"{am}: {n:,} records differ from heap", am)
        if e.stale:
            fail(f"{am}: {len(e.stale)} stale exceptions", am)
    for am, e in rows.items():
        e.failed = "*" in failed or am in failed

    passed = not problems
    text = _render(rows, engines, check, base_failing, problems, notes, passed, commit=commit,
                   sample=sample, shards=shards, head=head, base=base, base_ref=base_ref,
                   run_url=run_url, wall_clock=wall_clock, corpus=corpus,
                   by_shard=by_shard, wanted=wanted)
    text = truncate(text, max_chars, run_url)
    summary = {"pass": passed, "problems": problems, "notes": notes, "shards": shards,
               "coverage": sample, "corpus": commit, "engines": {
                   am: {"ran": e.ran, "ok": e.ok, "failing": e.failing,
                        "incomplete": e.incomplete,
                        "vs_base": None if base_failing is None else e.failing - base_failing,
                        "counts": dict(e.counts), "differences": dict(e.differences),
                        "stale_exceptions": len(e.stale), "gate": not e.failed}
                   for am, e in rows.items()},
               "baseline": check.to_json() if check else None}
    details = {
        "Timing by shard": [{"shard": shard, "workload": workload, "engine": am, "seconds": seconds}
                            for am, e in rows.items() for (shard, workload), seconds in sorted(e.seconds.items())],
        "Stale exceptions": [{"engine": am, **entry} for am, e in rows.items() for entry in e.stale],
        "Failure examples": [{"engine": am, "class": cls, **entry,
                              "query": ccompare.query_text(corpus, entry["file"], entry["line"]) if corpus else "unavailable"}
                             for am, e in rows.items() for cls, entries in e.examples.items() for entry in entries],
    }
    return Report(text, passed, problems, summary, details)


def _vs_base(e: Engine, base_failing: Optional[int]) -> str:
    if base_failing is None:
        return "n/a"
    delta = e.failing - base_failing
    if not delta:
        return "±0"
    pp = -100 * delta / e.ran if e.ran else 0
    return f"{pp:+.3f} pp ({delta:+,} failing)"


def _render(rows, engines, check, base_failing, problems, notes, passed, *, commit, sample,
            shards,
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
        cells = [ccompare._pct(e.ok, e.ran), _vs_base(e, base_failing)]
        if am == "heap":
            cells += ["—", f"{e.counts['postgres_semantics']:,}", "—", "—", "—",
                      f"{e.counts['crashed']:,}", f"{e.counts['timeout']:,}",
                      f"{e.counts['other']:,}"]
            gate = ("baseline: missing" if check is None else
                    f"baseline: {len(check.new) + len(check.changed):,} new, "
                    f"{len(check.fixed):,} fixed"
                    + (f", {len(check.incomplete):,} incomplete" if check.incomplete else "")
                    + (" (FAIL)" if e.failed else ""))
        else:
            cells += [f"{e.counts[c]:,}" for c in COLUMNS] + [f"{e.counts['other']:,}"]
            gate = "pass" if not e.failed and e.ran else "FAIL"
        out.append(f"| {e.label or am} | " + " | ".join(cells) + f" | {gate} |")
    out.append("")
    if problems:
        out.append("**Problems**\n")
        out += [f"- {p}" for p in problems[:40]]
        if len(problems) > 40:
            out.append(f"- ... {len(problems) - 40} more")
        out.append("")
    if notes:
        out += [f"_Note: {n}._" for n in notes] + [""]

    # New failures, per class: the engines' differences, and heap's.
    groups: dict[str, list[tuple[str, dict]]] = defaultdict(list)
    sizes: Counter = Counter()
    for am in engines:
        for cls, examples in sorted(rows[am].examples.items()):
            groups[cls] += [(am, d) for d in examples]
        sizes.update(rows[am].differences)
    if check:
        heap = check.new + check.changed
        groups["heap regression"] += [("heap", d) for d in heap[:PER_CLASS]]
        sizes["heap regression"] += len(heap)
    n = sum(sizes.values())
    out.append(f"<details><summary>New failures ({n:,})</summary>\n")
    if not n:
        out.append("None.\n")
    order = [c for c in ccompare.CLASSES if c in groups] + \
        sorted(c for c in groups if c not in ccompare.CLASSES)
    for cls in order:
        items = groups[cls]
        out.append(f"#### {cls}: {sizes[cls]:,}\n")
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
        if sizes[cls] > len(items[:PER_CLASS]):
            out.append(f"- ... {sizes[cls] - len(items[:PER_CLASS]):,} more")
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
