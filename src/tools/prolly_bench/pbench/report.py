"""Summaries of result sets, and comparison against a baseline.

Medians and spread (min-max) over repetitions, per (workload, size, variant,
clients).  S results are compared with B0 by normalizing each median to the
baseline's (B0 = 1.0); V results are reported as they are.
"""

from __future__ import annotations

import json
from collections import OrderedDict
from pathlib import Path
from typing import Any, Optional

from pbench.core.stats import summarize
from pbench.core.validation import cell_problems, compatible, validate_run

# Metrics shown in summaries, in this order, when a workload produces them.
S_METRICS = [
    "tps", "rows_per_second", "copy_seconds", "index_seconds", "checkpoint_seconds",
    "qps", "latency_p50_ms", "latency_p99_ms", "latency_p999_ms",
    "failed_transactions", "ignored_errors",
    "wal_bytes_per_operation", "wal_fpi", "device_write_bytes_per_operation",
    "host_busy_cpu_seconds", "host_hash_sample_percent", "io_hit_ratio", "io_evictions",
    "cluster_allocated_bytes_including_wal", "vacuum_seconds", "process_restart_seconds",
    "latency_samples", "hot_query_seconds", "client_process_qps", "skipped_transactions",
    "wa_server", "wa_wal", "wa_pages", "wa_tree",
]
V_METRICS = [
    "branch_create_ms", "checkout_ms", "change_ms", "commit_ms", "diff_ms",
    "merge_ms", "clone_ms", "fetch_ms", "push_ms", "background_tps",
    "wal_bytes", "device_write_bytes", "host_hash_sample_percent",
    "wa.server_bytes_per_operation", "wa.tree.file_bytes_per_operation",
]
C_METRICS = [
    "pass_percent", "records", "ok", "not_ok", "not_implemented", "crashed",
    "timeout", "did_not_run", "skipped", "files", "reruns",
]
# The write_amp collector's components (specs/19), as bytes per completed
# unit: (metric, heading).  The server total is WAL + pages + chunk store.
WA_COMPONENTS = [
    ("wa.logical_bytes_per_operation", "logical"),
    ("wa.wal.row_bytes_per_operation", "WAL rows"),
    ("wa.wal.index_bytes_per_operation", "WAL indexes"),
    ("wa.wal.fpi_bytes_per_operation", "WAL FPIs"),
    ("wa.wal.blockmap_bytes_per_operation", "WAL block map"),
    ("wa.wal.chunk_bytes_per_operation", "WAL chunks"),
    ("wa.wal.other_bytes_per_operation", "WAL other"),
    ("wa.pages.write_bytes_per_operation", "pages"),
    ("wa.tree.file_bytes_per_operation", "chunk store"),
    ("wa.server_bytes_per_operation", "server total"),
    ("wa_server", "WA"),
]
# For comparisons: whether a larger value is better.
HIGHER_IS_BETTER = {"tps", "qps", "rows_per_second", "io_hit_ratio", "background_tps"}


def load_results(out_dir: Path) -> list[dict[str, Any]]:
    with open(out_dir / "results.jsonl") as f:
        return [json.loads(line) for line in f if line.strip()]


def _group(results: list[dict]) -> "OrderedDict[tuple, list[dict]]":
    groups: OrderedDict[tuple, list[dict]] = OrderedDict()
    for r in results:
        k = r["key"]
        groups.setdefault((k["workload"], k["size"], k["variant"], k["clients"]),
                          []).append(r)
    return groups


def _fmt(x: Optional[float]) -> str:
    if x is None:
        return "-"
    if x == 0:
        return "0"
    if abs(x) >= 1000:
        return f"{x:,.0f}"
    return f"{x:.3g}"


def _status(rs: list[dict]) -> str:
    counts: dict[str, int] = {}
    for r in rs:
        status = "invalid" if r.get("validation_errors") else r["status"]
        counts[status] = counts.get(status, 0) + 1
    return ", ".join(f"{s} {n}/{len(rs)}" if s != "ok" else f"ok {n}/{len(rs)}"
                     for s, n in counts.items())


def summary_tables(results: list[dict], suite: str) -> str:
    wanted = {"S": S_METRICS, "V": V_METRICS, "C": C_METRICS}[suite]
    by_workload: OrderedDict[str, list] = OrderedDict()
    for key, rs in _group(results).items():
        by_workload.setdefault(key[0], []).append((key, rs))

    out = []
    for workload, groups in by_workload.items():
        present = [m for m in wanted
                   if any(r["metrics"].get(m) is not None for _, rs in groups for r in rs)]
        out.append(f"### {workload}\n")
        out.append("Cells show the median over repetitions, with min-max below it.\n")
        header = ["size", "variant", "clients", "status"] + present
        out.append("| " + " | ".join(header) + " |")
        out.append("|" + "---|" * len(header))
        for (w, size, variant, clients), rs in groups:
            row = [size, variant or "-", str(clients), _status(rs)]
            for m in present:
                if any(cell_problems(r) for r in rs):
                    row.append("INVALID")
                    continue
                s = summarize(r["metrics"].get(m) for r in rs if r["status"] == "ok")
                if s["median"] is None:
                    row.append("-")
                elif s["n"] > 1:
                    row.append(f"{_fmt(s['median'])}<br>{_fmt(s['min'])}–{_fmt(s['max'])}")
                else:
                    row.append(_fmt(s["median"]))
            out.append("| " + " | ".join(row) + " |")
        messages = sorted({r["message"] for _, rs in groups for r in rs
                           if r["status"] != "ok" and r["message"]})
        for msg in messages[:5]:
            out.append(f"\n- {msg}")
        out.append("")
    return "\n".join(out)


def wa_breakdown(results: list[dict]) -> list[tuple[tuple, list[Optional[float]]]]:
    """[(cell key, medians of WA_COMPONENTS)] for the cells write_amp measured."""
    rows = []
    for key, rs in _group(results).items():
        ok = [r for r in rs if r["status"] == "ok" and not cell_problems(r)]
        if not ok or not any("wa.server_bytes" in r["metrics"] for r in ok):
            continue
        rows.append((key, [summarize(r["metrics"].get(m) for r in ok)["median"]
                           for m, _ in WA_COMPONENTS]))
    return rows


def _wa_table(results: list[dict]) -> str:
    rows = wa_breakdown(results)
    if not rows:
        return ""
    out = ["## Write amplification\n",
           "Bytes written per completed unit over the write_amp interval (the window, "
           "then prolly's fold and a checkpoint), medians over repetitions; WA is the "
           "server total over the logical bytes (specs/19-write-amplification.md).\n",
           "| workload | size | variant | clients | " +
           " | ".join(h for _, h in WA_COMPONENTS) + " |",
           "|" + "---|" * 4 + "---:|" * len(WA_COMPONENTS)]
    for (w, size, variant, clients), values in rows:
        out.append("| " + " | ".join([w, size, variant or "-", str(clients)] +
                                     [_fmt(v) for v in values]) + " |")
    return "\n".join(out) + "\n"


def _collector_table(results: list[dict]) -> str:
    seen: dict[str, set] = {}
    for r in results:
        for name, (status, reason) in r["collector_status"].items():
            seen.setdefault(name, set()).add(f"{status}: {reason}")
    if not seen:
        return "Every collector ran in every cell.\n"
    lines = ["| collector | status |", "|---|---|"]
    for name in sorted(seen):
        lines.append(f"| {name} | {'<br>'.join(sorted(seen[name]))} |")
    return "\n".join(lines) + "\n"


def _environment(env: dict) -> str:
    cpu = {d["field"].rstrip(":"): d["data"] for d in env.get("cpu", {}).get("lscpu", [])}
    fs = (env.get("filesystem", {}).get("filesystems") or [{}])[0]
    lines = [
        f"- Host: {env.get('hostname')}, {cpu.get('Model name', '?')}, "
        f"{cpu.get('CPU(s)', '?')} CPUs, {(env.get('memory_kb') or 0) / 2**20:.0f} GiB RAM",
        f"- Kernel: {env.get('kernel', {}).get('release')}; "
        f"governor {env.get('cpu_governors')}, boost {env.get('cpu_boost')}, "
        f"EPP {env.get('cpu_energy_preference')}",
        f"- Data directory: {env.get('data_root')} on {fs.get('source')} "
        f"({fs.get('fstype')}, {fs.get('options')}); attributes "
        f"`{(env.get('data_root_attributes') or '?').split()[0]}`",
        f"- Postgres: {env.get('postgres', {}).get('version')}, optimization "
        f"`{' '.join(f for f in env.get('postgres', {}).get('cflags', '').split() if f.startswith(('-O', '-g', '-march')))}`"
        f"{', assertions on' if '--enable-cassert' in env.get('postgres', {}).get('configure', '') else ''}",
        f"- Source: {env.get('repo', {}).get('commit', '?')[:12]} "
        f"({env.get('repo', {}).get('dirty_files', '?')} uncommitted files)",
    ]
    if "sqllogictest" in env:
        lines.append(f"- sqllogictest: {env['sqllogictest'].get('commit', '?')[:12]} "
                     f"at {env['sqllogictest'].get('path')}; slt-runner source "
                     f"{(env.get('slt_source_hash') or '?')[:12]}")
    if "benchbase_source" in env:
        lines.append(f"- BenchBase: {env['benchbase_source'].get('commit', '?')[:12]} "
                     f"({env['benchbase_source'].get('branch')})")
    fio = env.get("fio", {})
    if fio.get("status") == "ok":
        parts = []
        for name in ("randwrite_4k", "seqwrite_1m", "fsync_8k"):
            j = fio.get(name, {})
            if "iops" in j:
                parts.append(f"{name} {j['iops']:,.0f} IOPS, {j['bw_bytes'] / 2**20:,.0f} MiB/s"
                             + (f", fsync mean {j['fsync_mean_us']:.0f} µs"
                                if "fsync_mean_us" in j else ""))
        lines.append("- fio: " + "; ".join(parts))
    elif fio:
        lines.append(f"- fio: {fio.get('status')} ({fio.get('reason', '')})")
    return "\n".join(lines) + "\n"


def write_readme(out_dir: Path) -> None:
    run = json.loads((out_dir / "run.json").read_text())
    cfg = json.loads((out_dir / "config.json").read_text())
    env = json.loads((out_dir / "environment.json").read_text())
    results = load_results(out_dir)
    r = cfg["run"]
    text = [
        f"# {run['suite']} suite: {run['label']} ({run['table_am']})\n",
        f"Started {run['started']}.\n",
        f"```\n{run['command']}\n```\n",
        f"Config `{cfg.get('source')}`: {r['repetitions']} repetitions, clients "
        f"{r['clients']}, warm-up {r['warmup_seconds']} s, measured "
        f"{r['measure_seconds']} s, sizes {r['sizes']}.  Postgres settings: "
        + ", ".join(f"`{k}={v}`" for k, v in cfg["postgres"].items()) + "\n",
        "## Environment\n", _environment(env),
    ]
    problems = validate_run(out_dir)
    text += ["## Evidence validation\n", "FAIL: " + "; ".join(problems) if problems else "Complete, intact execution evidence.",
             "Quality profile: " + cfg["run"].get("quality", "legacy/unverified") +
             ". Smoke results do not establish performance rankings.\n"]
    if run["warnings"]:
        text.append("## Warnings\n")
        text += [f"- {w}" for w in run["warnings"]]
        text.append("")
    text += ["## Collectors\n", _collector_table(results),
             "## Results\n", summary_tables(results, run["suite"]), _wa_table(results)]
    (out_dir / "README.md").write_text("\n".join(text))


def compare(baseline_dir: Path, other_dir: Path) -> str:
    """S results of other_dir normalized to baseline_dir (baseline = 1.0)."""
    problems = compatible(baseline_dir, other_dir)
    if problems:
        return "Comparison withheld:\n\n" + "\n".join(f"- {p}" for p in problems) + "\n"
    base = _group(load_results(baseline_dir))
    other = _group(load_results(other_dir))
    out = [f"# {other_dir.name} relative to {baseline_dir.name}\n",
           "Median of each metric divided by the baseline's median; for metrics "
           "where higher is better (" + ", ".join(sorted(HIGHER_IS_BETTER)) +
           ") above 1.0 is better, for the rest below 1.0 is better.\n",
           "| workload | size | variant | clients | status | " +
           " | ".join(S_METRICS) + " |", "|" + "---|" * (5 + len(S_METRICS))]
    for key, rs in other.items():
        row = [key[0], key[1], key[2] or "-", str(key[3]), _status(rs)]
        b = base.get(key)
        for m in S_METRICS:
            mine = summarize(r["metrics"].get(m) for r in rs if r["status"] == "ok")
            theirs = summarize(r["metrics"].get(m) for r in (b or []) if r["status"] == "ok")
            if (mine["median"] is None or not theirs["median"] or mine["n"] != len(rs)
                    or theirs["n"] != len(b or [])):
                row.append("-")
            else:
                row.append(f"{mine['median'] / theirs['median']:.2f}")
        out.append("| " + " | ".join(row) + " |")
    return "\n".join(out) + "\n"
