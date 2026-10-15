"""prolly_bench: benchmark harness for prolly storage.

  prolly_bench run S --am heap                  S suite against stock heap (B0)
  prolly_bench run V --am prolly_d1             V suite against an engine
  prolly_bench run C --am heap,prolly           C suite (SQL correctness), heap first
  prolly_bench list                             workloads, collectors, sinks
  prolly_bench report DIR                       rewrite DIR/README.md
  prolly_bench compare BASELINE_DIR DIR         S results normalized to B0
  prolly_bench table [DIR ...]                  engines side by side, per metric
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
    from pbench.core.runner import Runner

    over: dict = {"run": {}, "postgres": {}}
    if args.workloads:
        over["run"]["workloads"] = args.workloads
    if args.sizes:
        over["run"]["sizes"] = args.sizes
    if args.clients:
        over["run"]["clients"] = args.clients
    if args.reps is not None:
        over["run"]["repetitions"] = args.reps
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

    # Paths in [tools] are relative to the repository root.
    for k, v in list(cfg["tools"].items()):
        p = Path(v).expanduser()
        cfg["tools"][k] = str(p if p.is_absolute() else REPO / p)

    # Several AMs run one after another, heap first, each its own result set.
    ams = sorted(dict.fromkeys(args.am), key=lambda am: am != "heap")
    if args.label and len(ams) > 1:
        raise SystemExit("--label names one result set; give one --am with it")
    ok = True
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
                        allow_heap_v=args.allow_heap_v, keep_failed=args.keep_failed)
        results = runner.run()
        ok = ok and all(r.status.value != "error" for r in results)
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
    return 0


def _compare(args) -> int:
    from pbench import report
    text = report.compare(args.baseline, args.dir)
    if args.output:
        args.output.write_text(text)
    else:
        print(text)
    return 0


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
    md = table.to_markdown(sets, suite)
    out = args.output or args.results / f"summary-{suite}"
    out.with_suffix(".md").write_text(md)
    out.with_suffix(".html").write_text(table.to_html(sets, suite))
    print(md)
    print(f"written: {out.with_suffix('.md')}\n         {out.with_suffix('.html')}")
    return 0


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
    r.add_argument("--allow-heap-v", action="store_true",
                   help="allow the V suite on heap (harness testing only)")
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
    t.add_argument("--suite", choices=["S", "V"], default="S")
    t.add_argument("--results", type=Path, default=REPO / "specs" / "results")
    t.add_argument("-o", "--output", type=Path,
                   help="output path without extension (default RESULTS/summary-SUITE)")
    t.set_defaults(fn=_table)

    args = p.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
