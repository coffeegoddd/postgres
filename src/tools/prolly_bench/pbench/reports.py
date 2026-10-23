"""Assemble readable reports from persisted evidence, independent of format."""
from __future__ import annotations

import json
import re
import subprocess
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

from pbench import bcompare, report, table
from pbench.core import registry, suites
from pbench.core.validation import cell_problems, compatible, validate_run
from pbench.presentation import Document, Section, Value, link


@dataclass(frozen=True)
class Metric:
    key: str
    title: str
    unit: str
    direction: str = "neutral"


_LOWER = {"latency_p50_ms", "latency_p99_ms", "latency_p999_ms", "copy_seconds",
          "index_seconds", "checkpoint_seconds", "hot_query_seconds", "vacuum_seconds",
          "process_restart_seconds", "branch_create_ms", "checkout_ms", "change_ms",
          "commit_ms", "diff_ms", "merge_ms", "clone_ms", "fetch_ms", "push_ms",
          "wal_bytes_per_operation", "device_write_bytes_per_operation", "wa_server",
          "wa_wal", "wa_pages", "wa_tree", "cluster_allocated_bytes_including_wal",
          "relation_bytes", "wa.server_bytes_per_operation", "wa.tree.file_bytes_per_operation"}


def metrics(suite: str, keys: set[str]) -> list[Metric]:
    definitions = dict((k, (h, u)) for k, h, u in table.S_TABLES + table.V_TABLES)
    definitions.update({"copy_seconds": ("Bulk copy", "seconds"),
                        "index_seconds": ("Index build", "seconds"),
                        "checkpoint_seconds": ("Checkpoint", "seconds"),
                        "qps": ("Query throughput", "queries/s"),
                        "rows_per_second": ("Row throughput", "rows/s"),
                        "tps": ("Transaction throughput", "transactions/s"),
                        "change_ms": ("Change rows", "ms"),
                        "pass_percent": ("Pass rate", "%")})
    for key in report.C_METRICS:
        definitions.setdefault(key, (key.replace("_", " ").capitalize(), "count"))
    for key in report.S_METRICS + report.V_METRICS:
        unit = ("seconds" if key.endswith("_seconds") else "ms" if key.endswith("_ms") else
                "bytes" if "bytes" in key else "%" if key.endswith("percent") else "count")
        definitions.setdefault(key, (key.replace("_", " ").capitalize(), unit))
    for key, title in report.WA_COMPONENTS:
        definitions.setdefault(key, ("Write amplification: " + title, "bytes per completed unit"))
    layout = suites.layout(suite)
    preferred = ([k for k, *_ in table.S_TABLES] + report.S_METRICS if layout == suites.STORAGE else
                 [k for k, *_ in table.V_TABLES] + report.V_METRICS
                 if layout == suites.VERSION_CONTROL else report.C_METRICS)
    wanted = list(dict.fromkeys(preferred + sorted(keys)))
    return [Metric(k, *definitions.get(k, (k, "unit unspecified")),
                   "higher" if k in report.HIGHER_IS_BETTER | {"throughput"} else
                   "lower" if k in _LOWER else "neutral")
            for k in wanted if k in keys]


def read_json(path: Path, problems: list[str]) -> dict:
    try:
        result = json.loads(path.read_text())
        if not isinstance(result, dict):
            raise ValueError("expected an object")
        return result
    except (OSError, ValueError) as e:
        problems.append(f"{path.name}: {e}")
        return {}


@dataclass
class Source:
    path: Path
    run: dict
    config: dict
    environment: dict
    results: list[dict]
    problems: list[str] = field(default_factory=list)

    @property
    def label(self) -> str:
        return self.run.get("label", self.path.name)

    def result_set(self) -> table.ResultSet:
        return table.ResultSet(self.path, self.run.get("suite", "?"), self.label,
                               self.run.get("table_am", "?"), self.run.get("started", ""),
                               self.results, self.problems, self.run.get("build", ""),
                               self.run.get("builds", []))


def load(path: Path) -> Source:
    try:
        problems = validate_run(path)
    except (AttributeError, TypeError, KeyError) as e:
        problems = [f"invalid result evidence: {e}"]
    run = read_json(path / "run.json", problems)
    config = read_json(path / "config.json", problems)
    env = read_json(path / "environment.json", problems)
    for obj, fields in ((config, ("run", "postgres")), (env, ("postgres", "repo", "kernel"))):
        for name in fields:
            if name in obj and not isinstance(obj[name], dict):
                problems.append(f"invalid object field: {name}")
                obj[name] = {}
    for name in ("suite", "label", "table_am", "started", "build"):
        if name in run and not isinstance(run[name], str):
            problems.append(f"invalid run field: {name}")
            run[name] = str(run[name])
    for name in ("planned_cells", "warnings", "builds"):
        expected = dict if name == "planned_cells" else str
        if name in run and (not isinstance(run[name], list) or any(not isinstance(v, expected) for v in run[name])):
            problems.append(f"invalid run field: {name}")
            run[name] = []
    for key in run.get("planned_cells", []):
        problems.extend(f"planned cell: {error}" for error in _key_errors(key, run.get("suite")))
    results = []
    try:
        for n, line in enumerate((path / "results.jsonl").read_text().splitlines(), 1):
            if not line.strip():
                continue
            try:
                r = json.loads(line)
                if not isinstance(r, dict) or not isinstance(r.get("key"), dict) or not isinstance(r.get("metrics"), dict):
                    raise ValueError("expected cell key and metrics objects")
                if not isinstance(r.get("measurement", {}), dict) or not isinstance(r.get("raw_dir", ""), str):
                    raise ValueError("expected measurement object and raw path string")
                if any(not isinstance(v, (str, int, float, type(None))) for v in r["key"].values()):
                    raise ValueError("invalid cell key")
                problems.extend(f"results.jsonl line {n}: {error}" for error in _key_errors(r["key"], run.get("suite")))
                statuses = r.get("collector_status", {})
                if not isinstance(statuses, dict):
                    statuses = {"collector_status": statuses}
                safe_statuses = {}
                for name, value in statuses.items():
                    if not isinstance(value, (list, tuple)) or len(value) != 2 or not all(isinstance(v, str) for v in value):
                        reason = f"invalid collector_status {name}: {value!r}"
                        problems.append(f"results.jsonl line {n}: {reason}")
                        value = ("invalid", reason)
                    safe_statuses[name] = value
                r["collector_status"] = safe_statuses
                results.append(r)
            except (ValueError, TypeError) as e:
                problems.append(f"results.jsonl line {n}: {e}")
    except OSError as e:
        problems.append(f"results.jsonl: {e}")
    return Source(path, run, config, env, results, list(dict.fromkeys(problems)))


def _key_errors(key: dict, suite: str | None) -> list[str]:
    errors = []
    if not suites.is_workload_id(key.get("workload")):
        errors.append("missing or invalid workload in cell key")
    for name, kind in (("size", str), ("variant", str), ("clients", int), ("rep", int)):
        # Older C evidence names only the workload. Build comparisons need the
        # complete S/V key; never fill in missing repetition IDs for inference.
        if (suites.layout(suite) in (suites.STORAGE, suites.VERSION_CONTROL) or name in key) \
                and type(key.get(name)) is not kind:
            errors.append(f"missing or invalid {name} in cell key")
    return errors


def _key(k: dict) -> tuple:
    return (str(k.get("workload") or "?"), str(k.get("size") or "-"),
            str(k.get("variant") or "-"), str(k.get("clients", 0)))


def _natural(value) -> tuple:
    return tuple((1, int(p)) if p.isdigit() else (0, p)
                 for p in re.split(r"([0-9]+)", str(value)))


def _groups(source: Source) -> dict[tuple, list[dict]]:
    groups = {_key(k): [] for k in source.run.get("planned_cells", [])}
    for r in source.results:
        groups.setdefault(_key(r["key"]), []).append(r)
    return groups


def _planned(source: Source, key: tuple) -> int:
    return sum(_key(k) == key for k in source.run.get("planned_cells", []))


def _fmt(value) -> str:
    return "—" if value is None else table._fmt(value)


def _metric_value(source: Source, key: tuple, rows: list[dict], metric: Metric,
                  base: list[dict] | None = None) -> Value:
    if not rows:
        return Value("missing", f"0/{_planned(source, key)} planned repetitions")
    states = Counter(r.get("status", "missing status") for r in rows)
    if any(state != "ok" for state in states):
        return Value(", ".join(f"{s} {n}/{len(rows)}" for s, n in sorted(states.items())),
                     "; ".join(dict.fromkeys(r.get("message", "") for r in rows)))
    if source.problems:
        return Value("INVALID", "Evidence failed validation; see coverage and repetition details")
    unit_problem = base and {r.get("unit") for r in rows} != {r.get("unit") for r in base}
    c = table._cell(rows, metric.key, None if unit_problem else base)
    if unit_problem:
        c.detail += "; comparison withheld: different completed units"
    if not c.ok:
        status = c.text or "not measured"
        if status.startswith("missing"):
            reasons = sorted({f"{name}: {status}: {reason}"
                              for r in rows for name, (status, reason) in r.get("collector_status", {}).items()})
            c.detail = "; ".join(reasons) or "Metric absent in one or more repetitions"
        return Value(status, c.detail)
    text, detail = c.text, c.detail
    if c.ratio is not None:
        text += f" ({c.ratio:.2f}x)"
        detail += "; candidate/reference; median of paired ratios when an interval is shown, otherwise ratio of medians"
    tone = ""
    if metric.direction != "neutral" and c.better is not None:
        tone = "good" if c.better else "bad"
    if metric.direction == "neutral":
        detail += "; observational metric, no better/worse direction"
    return Value(text, detail, tone)


def _coverage(source: Source, output_dir: Path) -> Section:
    section = Section(f"Coverage: {source.label}", ["Field", "Value"])
    run, env = source.run, source.environment
    cfg = source.config.get("run", {})
    metadata = [("Result set", link(source.path / "run.json", output_dir, source.path.name)),
                ("Suite / engine / build", f"{run.get('suite', '?')} / {run.get('table_am', '?')} / {run.get('build') or 'default'}"),
                ("Started", run.get("started", "?")), ("Execution state", run.get("state", "legacy/unverified")),
                ("Quality", cfg.get("quality", "legacy/unverified")),
                ("Evidence", "INVALID: " + "; ".join(source.problems) if source.problems else "Complete, intact execution evidence"),
                ("Cells: planned / finished / successful", f"{len(run.get('planned_cells', []))} / {len(source.results)} / "
                 f"{sum(not cell_problems(r) for r in source.results)}"),
                ("Command", run.get("command", "not recorded")),
                ("Host / kernel", f"{env.get('hostname', '?')} / {env.get('kernel', {}).get('release', '?')}"),
                ("PostgreSQL", env.get("postgres", {}).get("version", "?")),
                ("Source commit", (env.get("postgres", {}).get("build_provenance") or {}).get("commit") or env.get("repo", {}).get("commit", "?")),
                ("PostgreSQL settings", json.dumps(source.config.get("postgres", {}), sort_keys=True)),
                ("Configuration", link(source.path / "config.json", output_dir, "config.json")),
                ("Environment and fio", link(source.path / "environment.json", output_dir, "environment.json")),
                ("Cell evidence", link(source.path / "results.jsonl", output_dir, "results.jsonl"))]
    section.rows = [[k, v] for k, v in metadata]
    section.notes += run.get("warnings", [])
    return section


def _build_comparisons(sources: list[Source], document: Document) -> None:
    base = bcompare.base_build([s.result_set() for s in sources])
    if not base:
        return
    for candidate in sorted({s.run.get("build", "") for s in sources} - {base, ""}):
        selected = [s for s in sources if s.run.get("build") in (base, candidate)]
        section = Section(f"Candidate over base: {candidate}/{base}", ["Workload", "Variant", "Size", "Clients", "Engine"] +
                      [h for _, h, _ in bcompare.METRICS] + ["prolly/heap base → candidate", "Result"],
                      notes=["Median of paired ratios with 95% bootstrap intervals; candidate over base. "
                             "Verdicts use the existing 5% practical effect threshold."])
        problems = [f"{s.label}: {p}" for s in selected for p in s.problems]
        try:
            rows = bcompare.compare([s.result_set() for s in selected]) if not problems else []
        except (KeyError, ValueError, TypeError) as e:
            problems.append(f"invalid build comparison inputs: {e}")
            rows = []
        if problems:
            section.notes.extend(f"Comparison withheld: {p}" for p in problems)
            document.passed = False
            document.sections.append(section)
            continue
        benchmark = all(r.get("measurement", {}).get("quality") == "benchmark" for s in selected for r in s.results)
        for r in rows:
            if not benchmark:
                for ratio in r.ratios.values():
                    ratio.low = ratio.high = None
            section.rows.append([r.workload, r.variant or "-", r.size, str(r.clients), r.engine] +
                            [r.ratios[k].text() if not r.problems else "withheld" for k, *_ in bcompare.METRICS] +
                            [" → ".join(_fmt(v) for v in r.over_heap.values()) if not r.problems else "withheld",
                             "; ".join(r.problems) if r.problems else bcompare.verdict(r) if benchmark else "descriptive (smoke)"])
            document.passed &= not bool(r.problems)
        if rows:
            document.sections.append(section)


def _correctness(sources: list[Source], document: Document) -> None:
    from pbench import ccompare
    bases = [s for s in sources if s.run.get("table_am") == "heap"]
    section = Section("Correctness against heap", ["Engine", "Workload", *ccompare.CLASSES, "Gate"],
                      notes=["Pass rates against corpus answers and agreement with heap are different measures."])
    if len(bases) != 1 or len(sources) < 2:
        section.notes.append("Comparison unavailable: supply exactly one heap result and at least one engine; no correctness verdict established.")
        document.passed = False
    else:
        base = bases[0]
        for s in sources:
            if s is base:
                continue
            workloads = sorted({_key(k)[0] for k in s.run.get("planned_cells", [])} |
                               {_key(r["key"])[0] for r in s.results}, key=_natural)
            try:
                problems = [*base.problems, *s.problems]
                if problems:
                    comparisons = {}
                else:
                    problems, comparisons = ccompare.assess(base.result_set(), s.result_set(), workloads)
            except (OSError, ValueError, RuntimeError, KeyError, subprocess.TimeoutExpired) as e:
                problems, comparisons = [f"correctness comparison unavailable: {e}"], {}
            document.passed &= not bool(problems)
            section.notes += [f"{s.label}: {p}" for p in problems]
            for w in workloads:
                c = comparisons.get(w)
                section.rows.append([s.label, w] +
                                    [str(c["counts"].get(k, 0)) if c else "unavailable" for k in ccompare.CLASSES] +
                                    ["FAIL" if problems or not c else "PASS"])
    document.sections.append(section)


def build(sources: list[Source], output_dir: Path, *, baseline: Path | None = None) -> Document:
    registry.load_all()
    if not sources:
        raise ValueError("no result sets")
    if len({s.label for s in sources}) != len(sources):
        raise ValueError("result labels must be unique; supply exact runs, not merged retries or shards")
    letters = {s.run.get("suite") for s in sources} - {None}
    if len(letters) > 1:
        raise ValueError("give result sets of one suite")
    suite = next(iter(letters), "?")
    layout = suites.layout(suite)
    if baseline is not None:
        if layout != suites.STORAGE:
            raise ValueError("an explicit baseline is supported only for storage suites, as the S suite")
        if sum(s.path.resolve() == baseline.resolve() for s in sources) != 1:
            raise ValueError("the explicit baseline must name exactly one loaded result set")
    sources = sorted(sources, key=lambda s: (s.run.get("table_am") != "heap", s.label))
    document = Document(f"{suite} suite benchmark results", [
        "Medians and min–max over repetitions. Missing and invalid evidence is never ranked. "
        "Smoke ratios are descriptive and do not establish performance differences.",
        "Execution evidence and correctness gates are reported separately. "
        "Retained files are linked relative to this report."], passed=not any(s.problems for s in sources))
    groups = {s.label: _groups(s) for s in sources}
    keys = sorted({k for g in groups.values() for k in g}, key=lambda k: tuple(_natural(v) for v in k))
    found = {k for s in sources for r in s.results for k in r["metrics"]}
    if layout == suites.STORAGE:
        found.add("throughput")
    definitions = metrics(suite, found)
    # Infer applicability per workload variant, across engines/client counts.
    # A metric absent in one repetition is still missing evidence; a metric
    # produced only by a different workload must not create false missing rows.
    producers = {m.key: set() for m in definitions}
    for source in sources:
        for result in source.results:
            key = _key(result["key"])
            for metric in definitions:
                if metric.key in result["metrics"] or metric.key == "throughput" and any(
                        k in result["metrics"] for k in ("tps", "rows_per_second")):
                    producers[metric.key].add((key[0], key[2]))

    def applicable(metric: Metric, key: tuple) -> bool:
        return not producers[metric.key] or (key[0], key[2]) in producers[metric.key]
    overview_keys = ({"throughput", "latency_p99_ms", "wal_bytes_per_operation", "wa_server",
                      "cluster_allocated_bytes_including_wal"} if layout == suites.STORAGE else
                     {k for k in found if k.endswith("_ms")} | {"background_tps"}
                     if layout == suites.VERSION_CONTROL else
                     {"records", "pass_percent", "crashed", "timeout", "did_not_run"})
    overview_metrics = [m for m in definitions if m.key in overview_keys]
    overview = Section("Workload overview", ["Workload", "Size", "Variant", "Clients", "Engine/build", "Repetitions"] +
                       [f"{m.title} ({m.unit})" for m in overview_metrics])
    overview.notes = [table._workload_title(w) for w in sorted({k[0] for k in keys}, key=_natural)]
    for key in keys:
        for s in sources:
            rows = groups[s.label].get(key, [])
            if key not in groups[s.label]:
                continue
            overview.rows.append([*[str(v) for v in key], s.label,
                                  f"{len(rows)}/{_planned(s, key)} planned"] +
                                 [_metric_value(s, key, rows, m) if applicable(m, key) else
                                  Value("n/a", "Metric not produced by this workload variant") for m in overview_metrics])
    document.sections.append(overview)
    for s in sources:
        document.sections.append(_coverage(s, output_dir))
    if layout == suites.CORRECTNESS:
        _correctness(sources, document)
    # Choose a heap reference within each build, never across builds. Explicit
    # compare commands may nominate an arbitrary reference result set.
    references = {}
    for s in sources:
        candidates = [b for b in sources if b.path.resolve() == baseline.resolve()] if baseline else [
            b for b in sources if b.run.get("table_am") == "heap" and b.run.get("build", "") == s.run.get("build", "")]
        if layout == suites.STORAGE and len(candidates) == 1 and candidates[0] is not s:
            b = candidates[0]
            vary = "build" if b.run.get("build", "") != s.run.get("build", "") else "engine"
            problems = compatible(b.path, s.path, vary)
            if problems:
                document.notes += [f"{s.label}: comparison withheld: {p}" for p in problems]
                document.passed = False
            else:
                references[s.label] = b
        elif layout == suites.STORAGE and len(candidates) > 1:
            document.notes.append(f"{s.label}: comparison withheld: ambiguous heap baseline")
            document.passed = False
    additional = {}
    for metric in definitions:
        if metric.unit == "unit unspecified":
            group = metric.key.split(".", 1)[0] if "." in metric.key else "other collectors"
            section = additional.setdefault(group, Section(f"Additional metrics: {group}",
                ["Metric", "Workload", "Size", "Variant", "Clients", "Engine/build", "Completed unit", "Value"],
                notes=["Unregistered metrics (unit unspecified). No performance ranking is assigned."], folded=True))
            for key in keys:
                for s in sources:
                    rows = groups[s.label].get(key, [])
                    if not any(metric.key in r["metrics"] for r in rows):
                        continue
                    units = sorted({r.get("unit", "unknown") for r in rows})
                    section.rows.append([metric.key, *[str(v) for v in key], s.label,
                                         ", ".join(units), _metric_value(s, key, rows, metric)])
            continue
        section = Section(f"{metric.title} ({metric.unit})",
                          ["Workload", "Size", "Variant", "Clients", "Completed unit"] + [s.label for s in sources],
                          notes=[f"Metric: {metric.key}. Direction: {metric.direction}. "
                                 "Ratios use the compatible reference within this build unless explicitly nominated."])
        for key in keys:
            if not applicable(metric, key):
                continue
            units = sorted({r.get("unit", "unknown") for s in sources for r in groups[s.label].get(key, [])})
            cells = []
            for s in sources:
                reference = references.get(s.label)
                cells.append(_metric_value(s, key, groups[s.label].get(key, []), metric,
                             groups[reference.label].get(key) if reference else None))
            section.rows.append([*[str(v) for v in key], ", ".join(units) or "not recorded", *cells])
        document.sections.append(section)
    document.sections.extend(additional.values())
    if any(s.run.get("build") for s in sources):
        _build_comparisons(sources, document)
    for s in sources:
        details = Section(f"Repetition details: {s.label}",
                          ["Cell", "Status", "Unit / completed", "Metric", "Value", "Notes / scope", "Artifacts"],
                          notes=["Raw observations below are unverified when evidence validation fails; no performance verdict is implied."],
                          folded=True)
        for r in sorted(s.results, key=lambda r: json.dumps(r["key"], sort_keys=True)):
            notes = [r.get("message", ""), *r.get("notes", []), *r.get("validation_errors", [])]
            notes += [f"{k}: {v}" for k, v in sorted(r.get("measurement", {}).items())]
            notes += [f"{k}: {v[0]}: {v[1]}" for k, v in sorted(r.get("collector_status", {}).items())]
            raw = (s.path / r.get("raw_dir", "raw")).resolve()
            artifact = link(raw, output_dir, "raw output") if raw.is_relative_to(s.path.resolve()) else Value("invalid raw path")
            prefix = [json.dumps(r["key"], sort_keys=True), r.get("status", "unknown"),
                      f"{r.get('unit', '?')} / {r.get('denominator', '?')}"]
            for k, v in sorted(r["metrics"].items()):
                details.rows.append([*prefix, k, str(v), "; ".join(n for n in notes if n), artifact])
            if not r["metrics"]:
                details.rows.append([*prefix, "not measured", "—", "; ".join(n for n in notes if n), artifact])
        document.sections.append(details)
        queries = Section(f"TPC-H queries: {s.label}", ["Repetition", "Query", "Count", "Mean (ms)", "Total (ms)", "Evidence"])
        for r in s.results:
            if r["key"].get("workload") != "S13":
                continue
            raw = (s.path / r.get("raw_dir", "raw")).resolve()
            if not raw.is_relative_to(s.path.resolve()):
                continue
            path = (raw / "benchbase-measure" / "queries.json").resolve()
            if not path.is_relative_to(s.path.resolve()) or not path.exists():
                continue
            data = read_json(path, queries.notes)
            for name, values in sorted(data.items()):
                if not isinstance(values, dict) or any(type(v) not in (int, float) for v in values.values()):
                    queries.notes.append(f"Invalid per-query summary: {name}")
                    continue
                queries.rows.append([json.dumps(r["key"], sort_keys=True), name,
                                     str(values.get("count", "?")), _fmt(values.get("mean_ms")),
                                     _fmt(values.get("total_ms")), link(path, output_dir, "queries.json")])
        if queries.rows or queries.notes:
            if s.problems:
                queries.notes.append("Unverified observations: source evidence failed validation.")
            document.sections.append(queries)
    return document
