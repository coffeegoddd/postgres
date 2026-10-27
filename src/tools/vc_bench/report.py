"""Descriptive summaries and bootstrap log-log slopes, never Big-O proofs."""
import collections
import csv
import os
import json
import math
from pathlib import Path
import random
import statistics

from core import PASSES, write_json
from human_report import render

# What each instrumented pass reports.  Results from before the cpu and io
# passes recorded every counter in the memory pass: it reports all of them.
PASS_FIELDS = {
    "memory": ("pss_bytes_baseline", "pss_bytes_peak", "pss_bytes_peak_delta", "pss_bytes_retained_delta",
               "rss_bytes_peak", "private_bytes_peak_delta"),
    "cpu": ("cpu_seconds_delta", "major_faults_delta", "minor_faults_delta"),
    "io": ("read_bytes_delta", "write_bytes_delta"),
}
LEGACY_MEMORY_FIELDS = PASS_FIELDS["memory"] + PASS_FIELDS["cpu"] + PASS_FIELDS["io"]
CPU_SHORT_TICKS = 3

# A series drifts when its samples, in run order, rise or fall together
# more than chance allows: a two-sided Mann-Kendall test below DRIFT_P.  A
# report holds hundreds of series, so the bar is high; a cell's seven
# repetitions rising every time clear it (p 0.0004), six of them don't.
# Too few samples (the default single repetition) can't.
DRIFT_P = 0.001
DRIFT_MIN_SAMPLES = 5


def _mahonian(n):
    """Permutations of n items by their number of inversions."""
    counts = [1]
    for m in range(1, n + 1):
        out = [0] * (len(counts) + m - 1)
        for k, c in enumerate(counts):
            for j in range(m):
                out[k + j] += c
        counts = out
    return counts


def trend(values):
    """Mann-Kendall trend of values in their order: (S, tau, two-sided p),
    or None when every value is the same.  Exact without ties; with them
    (common for counters), the normal approximation with its tie
    correction."""
    n = len(values)
    pairs = n * (n - 1) // 2
    s = sum((b > a) - (b < a) for i, a in enumerate(values) for b in values[i + 1:])
    groups = collections.Counter(values).values()
    if len(groups) == 1:
        return None
    if len(groups) == n and n <= 50:
        counts = _mahonian(n)
        # (S = pairs - 2 * inversions; symmetric about 0)
        extreme = sum(counts[:(pairs - abs(s)) // 2 + 1])
        p = min(1.0, 2 * extreme / math.factorial(n))
    else:
        var = (n * (n - 1) * (2 * n + 5) - sum(t * (t - 1) * (2 * t + 5) for t in groups)) / 18
        z = max(0, abs(s) - 1) / math.sqrt(var)
        p = math.erfc(z / math.sqrt(2))
    return s, s / pairs, p


def drift(rows, fields):
    """Series whose measured samples drift with run order (rows in the order
    they were measured): each cell's latency (timing pass) and each of its
    engine counters (counters pass), where any trend at all is work that
    depends on what ran before, since the same operation counts the same."""
    series = collections.defaultdict(list)
    for row in rows:
        key = tuple(row[k] for k in fields)
        if row["pass"] == "timing":
            series[key + ("elapsed_ns",)].append(row["elapsed_ns"])
        elif row["pass"] == "counters":
            series[key + ("counters",)].append(row.get("counters") or {})
    flags = []
    for key, samples in sorted(series.items(), key=lambda item: str(item[0])):
        if key[-1] == "elapsed_ns":
            named = [("elapsed_ns", samples)]
        else:
            names = sorted({name for s in samples for name in s})
            named = [("counter_" + name, [s.get(name, 0) for s in samples]) for name in names]
        for metric, values in named:
            if len(values) < DRIFT_MIN_SAMPLES:
                continue
            result = trend(values)
            if result is None or result[2] >= DRIFT_P:
                continue
            s, tau, p = result
            flags.append({**dict(zip(fields, key[:-1])), "metric": metric, "samples": len(values),
                          "direction": "rising" if s > 0 else "falling", "kendall_tau": tau,
                          "p_value": p, "first": values[0], "last": values[-1]})
    return flags


def merge(paths, results_root):
    """One results directory holding the samples of several complete runs of one
    configuration, each of other passes (run --passes ...), for report()."""
    import datetime
    import shutil
    import uuid
    manifests = [json.loads((Path(p) / "manifest.json").read_text()) for p in paths]
    for p, m in zip(paths, manifests):
        if m["status"] != "complete":
            raise ValueError(f"{p}: run is incomplete or failed; refusing to merge it")
    def same(m):
        c = dict(m["config"])
        c.pop("passes", None)
        return c
    base = same(manifests[0])
    for p, m in zip(paths[1:], manifests[1:]):
        if same(m) != base:
            raise ValueError(f"{p}: configuration differs from {paths[0]} beyond its passes; refusing to merge")
    passes = [x for m in manifests for x in m["config"]["passes"]]
    if len(set(passes)) != len(passes):
        raise ValueError("the runs measure a pass more than once; merge runs of different passes")
    binaries = {}
    for p, m in zip(paths, manifests):
        for engine, b in m["binaries"].items():
            if engine in binaries and binaries[engine]["sha256"] != b["sha256"]:
                raise ValueError(f"{p}: {engine} binary differs from another run's; refusing to merge")
            binaries.setdefault(engine, b)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:6]
    out = Path(results_root) / f"merged-{stamp}"
    out.mkdir(parents=True)
    merged = dict(manifests[0])
    merged["config"] = dict(manifests[0]["config"], passes=[x for x in PASSES if x in passes])
    merged["binaries"] = binaries
    merged["planned_samples"] = sum(m["planned_samples"] for m in manifests)
    merged["merged_from"] = [str(Path(p).resolve()) for p in paths]
    with (out / "samples.jsonl").open("w") as f:
        for p in paths:
            f.write((Path(p) / "samples.jsonl").read_text())
    for name in Path(paths[0]).iterdir():
        if name.name.endswith(("-fixture.json", "-server.log", "-settings.json")):
            shutil.copyfile(name, out / name.name)
    write_json(out / "manifest.json", merged)
    return out


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
    legacy = not ({"cpu", "io"} & set(manifest.get("config", {}).get("passes", ())))
    for row in good:
        # Preserve compatibility with results recorded before dirty VC scenarios.
        was_dirty = row['operation'] in ('diff_working', 'diff_working_history')
        row.setdefault('working_set', 'diff' if was_dirty else 'clean')
        row.setdefault('dirty_rows', row['changed_rows'] if was_dirty else 0)
        # (results from before the history window was checked: all local)
        row.setdefault('window', 'in')
    # (window: a target past the history window is a group of its own,
    # never in the window's medians)
    fields = ["engine", "operation", "rows", "changed_rows", "history_commits",
              "working_set", "dirty_rows", "distance", "window", "pass"]
    groups = collections.defaultdict(list)
    for row in good:
        groups[tuple(row[k] for k in fields)].append(row)
    summaries = []
    for key, records in sorted(groups.items()):
        summary = dict(zip(fields, key))
        times = [r["elapsed_ns"]/1e6 for r in records]
        summary.update(repetitions=len(records), median_ms=statistics.median(times),
                       min_ms=min(times), max_ms=max(times))
        # The path each repetition took, beside its latency: the root it
        # was checked to have, and the path counters' growth.
        roots = sorted({str(r.get("target_root")) for r in records if r.get("target_root")})
        if roots:
            summary["target_root"] = "/".join(roots)
            summary["root_distance"] = max(r.get("root_distance") or 0 for r in records)
        if all("path" in r for r in records):
            for name in sorted({n for r in records for n in r["path"]}):
                summary["median_path_" + name] = statistics.median(r["path"].get(name, 0) for r in records)
        if key[-1] in PASS_FIELDS:
            measured = (LEGACY_MEMORY_FIELDS if key[-1] == "memory" and legacy else PASS_FIELDS[key[-1]])
            for k in measured:
                values = [r["memory"][k] for r in records if r["memory"][k] is not None]
                summary["median_"+k] = statistics.median(values) if len(values) == len(records) else None
            if key[-1] == "memory":
                summary["short_memory_observations"] = sum(r["memory"]["short_operation"] for r in records)
            if key[-1] in ("cpu", "memory") and "cpu_seconds_delta" in measured:
                # CPU time is counted in clock ticks: an operation of a few
                # ticks or less mostly measures as 0 or one tick.
                tick_ns = 1e9 / os.sysconf("SC_CLK_TCK")
                summary["short_cpu_observations"] = sum(r["elapsed_ns"] < CPU_SHORT_TICKS * tick_ns
                                                        for r in records)
        if key[-1] == "counters":
            # (a counter an operation didn't move was recorded as nothing: 0)
            for name in sorted({n for r in records for n in r["counters"]}):
                values = [r["counters"].get(name, 0) for r in records]
                summary["median_counter_" + name] = statistics.median(values)
                summary["min_counter_" + name] = min(values)
                summary["max_counter_" + name] = max(values)
        summaries.append(summary)
    flags = drift(good, fields)
    write_json(path / "drift.json", flags)
    with (path / "drift.csv").open("w") as f:
        writer = csv.DictWriter(f, list(dict.fromkeys(k for s in flags for k in s)) or ["metric"])
        writer.writeheader()
        writer.writerows(flags)
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
        # (a dirty profile is held fixed by its label: its count varies with
        # the table's size if it's a fraction, so a size sweep holds the
        # fraction; across profiles, the count varies, not the label)
        fixed = [f for f in fields if f not in (axis, "pass")
                 and not (axis == 'dirty_rows' and f == 'working_set')
                 and not (axis == 'rows' and f == 'dirty_rows')]
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
    render(path, manifest, summaries, fits, flags)
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
    text += ["", f"Run-order drift (drift.csv): {len(flags)} series flagged."]
    for flag in flags:
        text.append(f"{flag['engine']} {flag['operation']} D={flag['distance']} {flag['working_set']} "
                    f"{flag['pass']} {flag['metric']}: {flag['direction']}, tau={flag['kendall_tau']:.2f}, "
                    f"p={flag['p_value']:.2g}")
    (path / "report.txt").write_text('\n'.join(text) + '\n')
    print('\n'.join(text))
