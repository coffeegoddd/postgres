"""prolly_bench: benchmark harness for prolly storage.

  prolly_bench run S --am heap                  S suite against stock heap (B0)
  prolly_bench run V --am prolly                V suite against an engine
  prolly_bench run C --am heap,prolly           C suite (SQL correctness), heap first
  prolly_bench list                             workloads, collectors, sinks
  prolly_bench report DIR                       rewrite DIR/README.md
  prolly_bench compare BASELINE_DIR DIR         S results normalized to B0
  prolly_bench table [DIR ...]                  engines side by side, per metric
  prolly_bench cdiff BASELINE_DIR DIR           C suite: one engine's differences from heap
"""

from __future__ import annotations

import argparse
import shlex
import sys
from pathlib import Path

from pbench.core import config as config_mod
from pbench.core import registry

REPO = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parents[1]


def _csv(conv):
    return lambda s: [conv(x) for x in s.split(",") if x]


def _run(args: argparse.Namespace) -> int:
    from pbench.core.runner import Runner, run_many

    over: dict = {"run": {}, "postgres": {}}
    if args.workloads:
        over["run"]["workloads"] = args.workloads
    if args.sizes:
        over["run"]["sizes"] = args.sizes
    if args.clients:
        over["run"]["clients"] = args.clients
    if args.reps is not None:
        over["run"]["repetitions"] = args.reps
    if args.quality:
        over["run"]["quality"] = args.quality
    if args.seed is not None:
        over["run"]["seed"] = args.seed
    if args.warmup is not None:
        over["run"]["warmup_seconds"] = args.warmup
    if args.measure is not None:
        over["run"]["measure_seconds"] = args.measure
    if args.no_fio:
        over["run"]["fio_baseline"] = False
    for s in args.set:
        k, _, v = s.partition("=")
        over["postgres"][k] = v
    config = args.config or HERE / "configs" / config_mod.DEFAULT_CONFIGS[args.suite]
    cfg = config_mod.load(config, over, args.suite)
    if not args.am:
        raise SystemExit("at least one --am is required")

    # Paths in [tools] are relative to the repository root.
    for k, v in list(cfg["tools"].items()):
        p = Path(v).expanduser()
        cfg["tools"][k] = str(p if p.is_absolute() else REPO / p)

    # Each engine retains its result set; matched cells run as randomized pairs.
    ams = sorted(dict.fromkeys(args.am), key=lambda am: am != "heap")
    if args.label and len(ams) > 1:
        raise SystemExit("--label names one result set; give one --am with it")
    ok = True
    out_dirs = []
    runners = []
    for am in ams:
        label = args.label
        if not label:
            label = "B0" if am == "heap" else am
            if cfg["postgres"].get("full_page_writes") == "off" and \
                    args.suite not in config_mod.DURABILITY_EXEMPT_SUITES:
                label += "-fpw-off"
        runner = Runner(suite=args.suite, cfg=cfg, table_am=am, label=label,
                        repo=REPO, bin_dir=args.pg_bin.resolve(),
                        work_dir=args.work_dir.resolve(), out_root=args.out.resolve(),
                        command="prolly_bench " + shlex.join(sys.argv[1:]),
                        keep_failed=args.keep_failed)
        runners.append(runner)
        out_dirs.append(runner.out_dir)
    from pbench.core.validation import validate_run
    all_results = run_many(runners)
    ok = all(bool(results) and not validate_run(runner.out_dir)
             for runner, results in zip(runners, all_results))
    if args.suite == "S" and "heap" in ams:
        from pbench.core.validation import compatible
        baseline = runners[ams.index("heap")].out_dir
        for runner in runners:
            if runner.table_am != "heap":
                problems = compatible(baseline, runner.out_dir)
                for problem in problems:
                    print(problem, file=sys.stderr)
                ok = ok and not problems
    if args.suite == "C" and len(out_dirs) > 1:
        from pbench import ccompare, table
        print("\n" + ccompare.to_markdown([table.load_set(d) for d in out_dirs]))
        sets = [table.load_set(d) for d in out_dirs]
        base = next((s for s in sets if s.table_am == "heap"), None)
        ok = ok and base is not None and all(not ccompare.gate(base, s,
            cfg["run"]["workloads"] or ["C1", "C2", "C3", "C4"])
            for s in sets if s is not base)
    return 0 if ok else 1


def _list(_args) -> int:
    registry.load_all()
    for suite in ("S", "V", "C"):
        print(f"{suite} suite workloads:")
        for name, cls in registry.workloads.items():
            if cls.suite == suite:
                print(f"  {name:<4} {cls.title}")
    print("collectors: " + ", ".join(registry.collectors.names()))
    print("sinks:      " + ", ".join(registry.sinks.names()))
    print("clusters:   " + ", ".join(registry.clusters.names()))
    return 0


def _report(args) -> int:
    from pbench import report
    report.write_readme(args.dir)
    print(args.dir / "README.md")
    from pbench.core.validation import validate_run
    return int(bool(validate_run(args.dir)))


def _compare(args) -> int:
    from pbench import report
    text = report.compare(args.baseline, args.dir)
    if args.output:
        args.output.write_text(text)
    else:
        print(text)
    from pbench.core.validation import compatible
    return int(bool(compatible(args.baseline, args.dir)))


def _cdiff(args) -> int:
    from pbench import ccompare, table
    text = ccompare.cdiff(table.load_set(args.baseline), table.load_set(args.dir),
                          args.classes, args.limit)
    if args.output:
        args.output.write_text(text)
    else:
        print(text)
    return 0


def _cgate(args) -> int:
    from pbench import ccompare, table
    problems = ccompare.gate(table.load_set(args.baseline), table.load_set(args.dir), args.workloads)
    for problem in problems:
        print(problem, file=sys.stderr)
    if not problems:
        print("C suite gate passed")
    return int(bool(problems))


def _gate(args) -> int:
    from pbench.core.validation import validate_run, compatible
    problems = []
    for path in args.dirs:
        problems.extend(f"{path}: {p}" for p in validate_run(path))
        if args.baseline:
            problems.extend(compatible(args.baseline, path))
    for problem in problems:
        print(problem, file=sys.stderr)
    if not problems:
        print("Execution evidence gate passed (correctness comparison uses cgate)")
    return int(bool(problems))


def _table(args) -> int:
    from pbench import table
    if args.dirs:
        sets = [table.load_set(d) for d in args.dirs]
        suites = {s.suite for s in sets}
        if len(suites) != 1:
            raise SystemExit("give result sets of one suite")
        suite = suites.pop()
    else:
        suite = args.suite
        sets = table.discover(args.results, suite)
    if not sets:
        raise SystemExit(f"no {suite} result sets under {args.results}")
    out = args.output or args.results / f"summary-{suite}"
    if suite == "C":
        from pbench import ccompare
        md = ccompare.to_markdown(sets)
        out.with_suffix(".md").write_text(md)
        print(md)
        print(f"written: {out.with_suffix('.md')}")
        base = next((s for s in sets if s.table_am == "heap"), None)
        workloads = sorted({r["key"]["workload"] for s in sets for r in s.results})
        return int(base is None or len(sets) < 2 or any(ccompare.gate(base, s, workloads)
                   for s in sets if s is not base))
    md = table.to_markdown(sets, suite)
    out.with_suffix(".md").write_text(md)
    out.with_suffix(".html").write_text(table.to_html(sets, suite))
    print(md)
    print(f"written: {out.with_suffix('.md')}\n         {out.with_suffix('.html')}")
    from pbench.core.validation import compatible
    base = next((s for s in sets if s.is_baseline), None)
    return int(any(s.problems for s in sets) or bool(base and any(
        compatible(base.path, s.path) for s in sets if s is not base)))


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="prolly_bench", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("run", help="run a suite")
    r.add_argument("suite", choices=["S", "V", "C"])
    r.add_argument("--am", required=True, type=_csv(str),
                   help="table AM under test (heap, prolly, ...); a list runs each, heap first")
    r.add_argument("--label", help="result set label (default B0 for heap, else the AM)")
    r.add_argument("--config", type=Path,
                   help="default configs/standard.toml for S and V, configs/c_standard.toml for C")
    r.add_argument("--workloads", type=_csv(str), help="e.g. S1,S2 (default: all)")
    r.add_argument("--sizes", type=_csv(str))
    r.add_argument("--clients", type=_csv(int))
    r.add_argument("--reps", type=int)
    r.add_argument("--quality", choices=["smoke", "benchmark"], help="benchmark rejects uncontrolled conditions")
    r.add_argument("--seed", type=int, help="reproducible workload and execution-order seed")
    r.add_argument("--warmup", type=int, help="warm-up seconds")
    r.add_argument("--measure", type=int, help="measured seconds")
    r.add_argument("--set", action="append", default=[], metavar="NAME=VALUE",
                   help="Postgres setting override (repeatable)")
    r.add_argument("--pg-bin", type=Path, default=REPO / "build-rel" / "inst" / "bin")
    r.add_argument("--work-dir", type=Path, default=REPO / "build-bench" / "work",
                   help="where clusters live: on the device under test")
    r.add_argument("--out", type=Path, default=REPO / "specs" / "results")
    r.add_argument("--no-fio", action="store_true", help="skip the fio device baseline")
    r.add_argument("--keep-failed", action="store_true",
                   help="keep the cluster of a cell that errors, for debugging")
    r.set_defaults(fn=_run)

    sub.add_parser("list", help="list workloads, collectors and sinks").set_defaults(fn=_list)

    rp = sub.add_parser("report", help="rewrite a result set's README.md")
    rp.add_argument("dir", type=Path)
    rp.set_defaults(fn=_report)

    c = sub.add_parser("compare", help="S results normalized to a baseline")
    c.add_argument("baseline", type=Path)
    c.add_argument("dir", type=Path)
    c.add_argument("-o", "--output", type=Path)
    c.set_defaults(fn=_compare)

    t = sub.add_parser("table", help="engines side by side: a table per metric")
    t.add_argument("dirs", type=Path, nargs="*",
                   help="result sets (default: the latest per label)")
    t.add_argument("--suite", choices=["S", "V", "C"], default="S")
    t.add_argument("--results", type=Path, default=REPO / "specs" / "results")
    t.add_argument("-o", "--output", type=Path,
                   help="output path without extension (default RESULTS/summary-SUITE)")
    t.set_defaults(fn=_table)

    d = sub.add_parser("cdiff", help="C suite: one engine's differences from heap")
    d.add_argument("baseline", type=Path)
    d.add_argument("dir", type=Path)
    d.add_argument("--class", dest="classes", type=_csv(str),
                   help="only these classes (default: all but agree and postgres_semantics)")
    d.add_argument("--limit", type=int, default=20, help="examples per group")
    d.add_argument("-o", "--output", type=Path)
    d.set_defaults(fn=_cdiff)

    gate = sub.add_parser("cgate", help="fail on incomplete or differing C-suite results")
    gate.add_argument("baseline", type=Path)
    gate.add_argument("dir", type=Path)
    gate.add_argument("--workloads", type=_csv(str), default=["C1", "C2", "C3", "C4"])
    gate.set_defaults(fn=_cgate)

    evidence = sub.add_parser("gate", help="validate complete, intact execution evidence and optional baseline compatibility")
    evidence.add_argument("dirs", nargs="+", type=Path)
    evidence.add_argument("--baseline", type=Path)
    evidence.set_defaults(fn=_gate)

    from pbench import complexity
    growth = sub.add_parser("complexity", help="gate node-work growth at fixed workload variant across dataset sizes")
    growth.add_argument("dir", type=Path)
    growth.add_argument("--workload", choices=["V1", "V2", "V3", "V4"], default="V2")
    growth.add_argument("--operation", default="prolly_commit")
    growth.add_argument("--counter", choices=["nodes_read", "nodes_written"], default="nodes_read")
    growth.add_argument("--max-exponent", type=float, required=True)
    growth.set_defaults(fn=complexity.run)

    from pbench import differential
    diff = sub.add_parser("differential", help="seeded transaction histories compared with heap")
    diff.add_argument("--am", default="prolly",
                      choices=["heap", "prolly"])
    diff.add_argument("--pg-bin", type=Path, required=True)
    diff.add_argument("--out", type=Path, required=True, help="new artifact directory")
    diff.add_argument("--runner", type=Path, default=REPO / "build-bench/differential/engine-diff")
    diff.add_argument("--seed", type=int, default=1)
    diff.add_argument("--seeds", type=int, default=10)
    diff.add_argument("--steps", type=int, default=28)
    diff.add_argument("--reduce", type=int, default=0, help="bounded reduction attempts on failure")
    diff.add_argument("--replay", type=Path, help="saved program.json or reduced.json")
    diff.add_argument("--self-test", action="store_true", help="also prove the comparator rejects a known mismatch")
    diff.set_defaults(fn=differential.run)

    from pbench import vc
    scenario = sub.add_parser("vc", help="version-control scenarios with explicit engine capabilities")
    scenario.add_argument("--am", default="prolly", help="comma-separated engine names")
    scenario.add_argument("--pg-bin", type=Path, required=True)
    scenario.add_argument("--out", type=Path, required=True, help="new artifact directory")
    scenario.add_argument("--runner", type=Path, default=REPO / "build-bench/vc/engine-vc")
    scenario.add_argument("--cases", type=Path, default=HERE / "slt/internal/vcscenario/cases",
                          help="scenario JSON file or directory; saved scenarios can be replayed here")
    scenario.add_argument("--self-test", action="store_true", help="also exercise native adapter negative controls")
    scenario.set_defaults(fn=vc.run)

    args = p.parse_args(argv)
    try:
        return args.fn(args)
    except (config_mod.ConfigError, OSError, ValueError, RuntimeError) as e:
        print(f"prolly_bench: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
