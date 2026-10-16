"""Gate backend node-work growth over N at fixed workload/changed-row count.

This measures an empirical exponent over the sampled range, not a proof
of an asymptotic bound. Missing or undersized sweeps are inconclusive/fail.
"""
import json
import math
import random
import statistics
from collections import defaultdict

from pbench.core.validation import validate_run


def slope(points):
    xs = [math.log(n) for n, _ in points]
    ys = [math.log1p(work) for _, work in points]
    mx, my = statistics.mean(xs), statistics.mean(ys)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)


def analyze(samples, limit):
    if len(samples) < 4 or max(samples) / min(samples) < 8 or any(len(v) < 5 for v in samples.values()):
        return {"passed": False, "reason": "need at least four N values spanning 8x and five repetitions per N"}
    if any(not math.isfinite(v) or v < 0 for values in samples.values() for v in values):
        return {"passed": False, "reason": "invalid node work"}
    if not any(v for values in samples.values() for v in values):
        return {"passed": False, "reason": "no node work observed; cannot establish complexity"}
    rng = random.Random(0)
    estimates = sorted(slope([(n, statistics.median(rng.choices(values, k=len(values))))
                              for n, values in sorted(samples.items())]) for _ in range(2000))
    low, high = estimates[49], estimates[1949]
    return {"passed": high <= limit, "exponent": slope([(n, statistics.median(v)) for n, v in sorted(samples.items())]),
            "bootstrap_95_interval": [low, high], "maximum_exponent": limit,
            "samples": samples, "scope": "backend node work over sampled N range at fixed variant"}


def run(args):
    problems = validate_run(args.dir)
    if problems:
        print(json.dumps({"passed": False, "problems": problems}, indent=2))
        return 1
    cfg = json.loads((args.dir / "config.json").read_text())
    groups = defaultdict(lambda: defaultdict(list))
    for line in (args.dir / "results.jsonl").read_text().splitlines():
        r = json.loads(line)
        k = r["key"]
        if k["workload"] != args.workload:
            continue
        work = []
        for path in (args.dir / r["raw_dir"]).glob(f"{args.workload.lower()}*-counters.json"):
            for op in json.loads(path.read_text()):
                if op["sql"].startswith("SELECT " + args.operation + "("):
                    work.append(op[args.counter])
        if not work:
            print(json.dumps({"passed": False, "reason": f"missing {args.operation} backend counters"}))
            return 1
        n = cfg["sizes"][k["size"]]["v_rows"]
        groups[(k["variant"], k["clients"])][n].append(statistics.median(work))
    reports = [{"variant": variant, "clients": clients, **analyze(samples, args.max_exponent)}
               for (variant, clients), samples in sorted(groups.items())]
    passed = bool(reports) and all(r["passed"] for r in reports)
    print(json.dumps({"passed": passed, "groups": reports}, indent=2))
    return int(not passed)
