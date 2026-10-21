"""Descriptive summaries and bootstrap log-log slopes, never Big-O proofs."""
import collections
import csv
import json
import math
from pathlib import Path
import random
import statistics

from core import write_json
from human_report import render


def slope(points):
    xs = [math.log(x) for x, _ in points]
    ys = [math.log(y) for _, y in points]
    xm, ym = statistics.mean(xs), statistics.mean(ys)
    xx = sum((x-xm)**2 for x in xs)
    b = sum((x-xm)*(y-ym) for x, y in zip(xs, ys)) / xx
    residual = sum((y-(ym+b*(x-xm)))**2 for x, y in zip(xs, ys))
    total = sum((y-ym)**2 for y in ys)
    return b, 1-residual/total if total else None


def fit(samples, seed=20261021):
    if len(samples) < 4 or max(samples)/min(samples) < 8 or min(map(len, samples.values())) < 5:
        return {"status": "inconclusive", "reason": "need >=4 distinct levels spanning >=8x, with >=5 repetitions each"}
    if any(y <= 0 for values in samples.values() for y in values):
        return {"status": "inconclusive", "reason": "nonpositive observations (often sampling resolution / zero memory growth)"}
    points = [(x, statistics.median(v)) for x, v in sorted(samples.items())]
    b, r2 = slope(points)
    rng = random.Random(seed)
    draws = []
    for _ in range(500):
        draws.append(slope([(x, statistics.median(rng.choices(v, k=len(v))))
                            for x, v in sorted(samples.items())])[0])
    draws.sort()
    return {"status": "descriptive", "exponent": b, "bootstrap_95_percent": [draws[12], draws[487]],
            "r_squared_log_space": r2, "levels": len(points), "span": max(samples)/min(samples),
            "warning": "Empirical power-law slope over this range; not an asymptotic bound. CI excludes between-run/systematic noise."}


def report(path):
    path = Path(path)
    manifest = json.loads((path / "manifest.json").read_text())
    rows = [json.loads(line) for line in (path / "samples.jsonl").read_text().splitlines()]
    good = [r for r in rows if r["status"] == "ok" and not r["warmup"]]
    if manifest["status"] != "complete" or len(good) != manifest["planned_samples"]:
        raise ValueError("Run is incomplete or failed; refusing to report a successful comparison. Inspect samples.jsonl and manifest.json.")
    for row in good:
        # Preserve compatibility with results recorded before dirty VC scenarios.
        was_dirty = row['operation'] in ('diff_working', 'diff_working_history')
        row.setdefault('working_set', 'diff' if was_dirty else 'clean')
        row.setdefault('dirty_rows', row['changed_rows'] if was_dirty else 0)
    fields = ["engine", "operation", "rows", "changed_rows", "history_commits",
              "working_set", "dirty_rows", "distance", "pass"]
    groups = collections.defaultdict(list)
    for row in good:
        groups[tuple(row[k] for k in fields)].append(row)
    summaries = []
    for key, records in sorted(groups.items()):
        summary = dict(zip(fields, key))
        times = [r["elapsed_ns"]/1e6 for r in records]
        summary.update(repetitions=len(records), median_ms=statistics.median(times),
                       min_ms=min(times), max_ms=max(times))
        if key[-1] == "memory":
            for k in ("pss_bytes_baseline", "pss_bytes_peak", "pss_bytes_peak_delta", "pss_bytes_retained_delta",
                      "rss_bytes_peak", "private_bytes_peak_delta", "cpu_seconds_delta", "major_faults_delta",
                      "minor_faults_delta", "read_bytes_delta", "write_bytes_delta"):
                values = [r["memory"][k] for r in records if r["memory"][k] is not None]
                summary["median_"+k] = statistics.median(values) if len(values) == len(records) else None
            summary["short_memory_observations"] = sum(r["memory"]["short_operation"] for r in records)
        summaries.append(summary)
    columns = list(dict.fromkeys(k for s in summaries for k in s))
    with (path / "summary.csv").open("w") as f:
        writer = csv.DictWriter(f, columns)
        writer.writeheader()
        writer.writerows(summaries)
    # A wide form makes direct engine comparisons usable without a spreadsheet pivot.
    comparison_fields = [f for f in fields if f != "engine"]
    comparisons = {}
    for summary in summaries:
        key = tuple(summary[f] for f in comparison_fields)
        row = comparisons.setdefault(key, dict(zip(comparison_fields, key)))
        for name, value in summary.items():
            if name not in fields:
                row[summary['engine'] + '.' + name] = value
    with (path / "comparison.csv").open("w") as f:
        columns = list(dict.fromkeys(k for s in comparisons.values() for k in s))
        writer = csv.DictWriter(f, columns)
        writer.writeheader()
        writer.writerows(comparisons.values())
    fits = []
    for axis in ("rows", "changed_rows", "distance", "dirty_rows"):
        fixed = [f for f in fields if f not in (axis, "pass")
                 and not (axis == 'dirty_rows' and f == 'working_set')]
        for metric in ("elapsed_ns", "pss_bytes_peak_delta", "pss_bytes_peak"):
            buckets = collections.defaultdict(lambda: collections.defaultdict(list))
            undersampled = set()
            for r in good:
                if r[axis] == 0 or r["pass"] != ("timing" if metric == "elapsed_ns" else "memory"):
                    continue
                key = tuple(r[k] for k in fixed)
                value = r[metric] if metric == "elapsed_ns" else r["memory"][metric]
                buckets[key][r[axis]].append(value)
                if metric != "elapsed_ns" and r["memory"]["short_operation"]:
                    undersampled.add(key)
            for key, samples in sorted(buckets.items()):
                result = fit(samples)
                if key in undersampled:
                    result = {"status": "inconclusive", "reason": "at least one operation too short for the memory sampling interval"}
                fits.append({"axis": axis, "metric": metric, **dict(zip(fixed, key)), **result})
    write_json(path / "scaling.json", fits)
    with (path / "scaling.csv").open("w") as f:
        columns = list(dict.fromkeys(k for s in fits for k in s))
        writer = csv.DictWriter(f, columns)
        writer.writeheader()
        writer.writerows(fits)
    render(path, manifest, summaries, fits)
    text = ["VC benchmark results", "", "Latency: persistent SQL request through complete result consumption; warm caches.",
            "Memory: separate instrumented pass, server-tree PSS/RSS. Sampled peaks are lower bounds.",
            "PSS peak delta measures resident growth, not total bytes allocated. Retained delta is immediate after the operation.",
            "Disk sizes include history and engine overhead; logical GB is decimal payload + two int64 columns.",
            "", "Human-readable comparisons: report.html / report.md.",
            "Side-by-side engines: comparison.csv. Long-form summary: summary.csv. Slopes: scaling.csv / scaling.json.",
            "The fitted model is T ~ X^exponent. An exponent near 0 suggests flat growth in this range; near 1 suggests linear growth.",
            "Timing data cannot prove Big-O. Fixed costs, cache capacity, GC, output size, and row locality can dominate.",
            "", "Descriptive timing slopes:"]
    for item in fits:
        if item["metric"] == "elapsed_ns" and item["status"] == "descriptive":
            text.append(f"{item['engine']} {item['operation']} vs {item['axis']}: exponent={item['exponent']:.3f}, CI={item['bootstrap_95_percent']}")
    if not any(f['status'] == 'descriptive' for f in fits):
        text.append("No sweep meets the sample/size requirements. Smoke runs test correctness only.")
    (path / "report.txt").write_text('\n'.join(text) + '\n')
    print('\n'.join(text))
