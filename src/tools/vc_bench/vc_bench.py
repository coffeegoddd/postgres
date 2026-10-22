#!/usr/bin/env python3
"""Build, provision, and measure Dolt, Doltgres and PostgreSQL prolly databases."""
import sys

if sys.version_info < (3, 11):
    sys.exit('vc_bench requires Python 3.11 or later')
if sys.platform != 'linux':
    sys.exit('vc_bench requires Linux (server lifecycle and /proc resource counters)')

import argparse
import fcntl
import json
from pathlib import Path

import core
import experiment
import report
import preflight


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", type=lambda s: Path(s).expanduser().resolve(), default=Path.home()/"dbs/benchmarks",
                   help="database, build, configuration, and results directory (default: ~/dbs/benchmarks)")
    sub = p.add_subparsers(dest="command", required=True)
    init = sub.add_parser("init", help="write an editable config without overwriting benchmark files")
    init.add_argument("--smoke", action="store_true", help="tiny fixtures for integration checks")
    for name in ("build", "prepare", "run", "start", "stop", "status", "check"):
        s = sub.add_parser(name)
        s.add_argument("--engines", default=','.join(core.ENGINES), help="comma-separated engine names")
        if name == "build":
            s.add_argument("--jobs", type=int, default=8)
        if name == "check":
            s.add_argument("--for", dest="stage", choices=("build", "prepare", "run", "start"),
                           default="prepare", help="prerequisites to check without starting work")
        if name == "run":
            s.add_argument("--passes", default=None,
                           help="comma-separated passes to run instead of the config's (" +
                                ", ".join(core.PASSES) + "); merge separate runs with report")
    s = sub.add_parser("report")
    s.add_argument("directory", type=Path, nargs="+",
                   help="results directory containing manifest.json; several (separate runs' passes "
                        "of one configuration) are merged into one report")
    s.add_argument("--out", type=Path, default=None,
                   help="where a merge of several runs is written (default: ROOT/results/merged-STAMP)")
    args = p.parse_args()
    if args.command == "init":
        core.init(args.root, args.smoke)
        return
    if args.command == "report":
        if len(args.directory) == 1:
            report.report(args.directory[0])
        else:
            report.report(report.merge(args.directory, args.out or args.root / "results"))
        return
    engines = args.engines.split(',')
    if len(set(engines)) != len(engines) or set(engines)-set(core.ENGINES):
        p.error("--engines must select distinct dolt,doltgres,prolly")
    c = core.config(args.root)
    # Advisory lock covers lifecycle and mutations across invocations of this tool.
    with (args.root / '.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError(f'Another vc_bench command is using {args.root}; wait for it to finish') from None
        if args.command == 'check':
            preflight.check(args.root, c, engines, args.stage)
            return
        if args.command == "build":
            if args.jobs <= 0:
                p.error("--jobs must be positive")
            if any(core.running(args.root, e) for e in engines):
                raise ValueError("Stop selected servers before rebuilding their binaries")
            if 'prolly' in engines and (args.root / 'build/postgres').exists():
                raise ValueError('PostgreSQL build directory already exists; use a fresh benchmark root to rebuild')
            preflight.check(args.root, c, engines, 'build')
            core.build(args.root, c, engines, args.jobs)
        elif args.command == "prepare":
            preflight.check(args.root, c, engines, 'prepare')
            experiment.prepare(args.root, c, engines)
        elif args.command == "run":
            if args.passes:
                c["passes"] = [p for p in args.passes.split(",") if p]
                core.check_passes(c["passes"])
            preflight.check(args.root, c, engines, 'run')
            out = experiment.run(args.root, c, engines)
            report.report(out)
        else:
            if args.command == 'start':
                preflight.check(args.root, c, engines, 'start')
            for engine in engines:
                if args.command == "status":
                    print(engine, json.dumps(core.running(args.root, engine)))
                else:
                    getattr(core, args.command)(args.root, c, engine)


if __name__ == "__main__":
    try:
        main()
    except (Exception, KeyboardInterrupt) as e:
        print(f"vc_bench: {type(e).__name__}: {e}", file=sys.stderr)
        sys.exit(1)
