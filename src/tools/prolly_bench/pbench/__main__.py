"""prolly_bench: benchmark harness for prolly storage.

  prolly_bench run S --am heap                  S suite against stock heap (B0)
  prolly_bench run V --am prolly                V suite against an engine
  prolly_bench run C --am heap,prolly           C suite (SQL correctness), heap first
  prolly_bench run C --shard 2/4 ...            one shard of the C suite's corpus
  prolly_bench cshard --shards 4                which files each shard runs
  prolly_bench cbaseline check DIR...           C suite: heap against the checked-in baseline
  prolly_bench creport DIR... --shards N ...    C suite: the pull-request comment
  prolly_bench list                             workloads, collectors, sinks
  prolly_bench report DIR                       regenerate HTML and Markdown reports
  prolly_bench compare BASELINE_DIR DIR         S results normalized to B0
  prolly_bench table [DIR ...]                  engines side by side, per metric
  prolly_bench cdiff BASELINE_DIR DIR           C suite: one engine's differences from heap
  prolly_bench scounters RUN RUN2               S counter runs of one build: unstable counters
  prolly_bench scalibrate --counters ... --timing ...   S gate: the noise of A/A runs
  prolly_bench sgate counters|timing RUN ...    S gate: a pull request against its base
  prolly_bench sreport --counters ... --out comment.md   S gate: the pull-request comment
  prolly_bench vgrowth RUN                      V suite: per-commit work growing with prior commits
  prolly_bench vshape RUN                       V suite: operations' work as N and Δ grow, against targets
  prolly_bench vclean RUN                       V suite: the same queries' rows and latency across cleanup
"""

from __future__ import annotations

import argparse
import shlex
import sys
from pathlib import Path

from pbench.core import config as config_mod
from pbench.core import registry, suites

REPO = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parents[1]


def _csv(conv):
    return lambda s: [conv(x) for x in s.split(",") if x]


def pg_bins(text: str) -> list[tuple[str, Path]]:
    """--pg-bin: one directory ([("", DIR)]), or builds to compare, the base
    first: base=DIR,candidate=DIR."""
    if "=" not in text:
        return [("", Path(text))]
    builds = []
    for part in text.split(","):
        name, sep, path = part.partition("=")
        if not sep or not name or not path or not name.replace("_", "").isalnum():
            raise SystemExit(f"--pg-bin: expected NAME=DIR, got {part!r}")
        builds.append((name, Path(path)))
    if len(builds) < 2 or len({n for n, _ in builds}) != len(builds):
        raise SystemExit("--pg-bin: give two or more builds, each named once")
    return builds


def build_commits(text: str | None, builds: list[tuple[str, Path]]) -> dict[str, str]:
    """--build-commit base=SHA,candidate=SHA: the commit (or a prefix of 7
    or more of its hex digits) each build's provenance must name."""
    if not text:
        return {}
    out = {}
    for part in text.split(","):
        name, sep, sha = part.partition("=")
        if not sep or len(sha) < 7 or any(c not in "0123456789abcdef" for c in sha.lower()):
            raise SystemExit(f"--build-commit: expected NAME=SHA (7 or more hex digits), got {part!r}")
        out[name] = sha.lower()
    unknown = set(out) - {name for name, _ in builds}
    if unknown:
        raise SystemExit(f"--build-commit names no build given to --pg-bin: {sorted(unknown)}")
    return out


def check_build_commits(builds: list[tuple[str, Path]], commits: dict[str, str]) -> None:
    """Refuse, before anything runs, a build whose provenance names another
    commit than --build-commit gives it."""
    from pbench.core.env import build_provenance
    for name, bin_dir in builds:
        if name not in commits:
            continue
        commit = (build_provenance(bin_dir.resolve()) or {}).get("commit") or ""
        if not commit.startswith(commits[name]):
            raise SystemExit(f"the {name} build at {bin_dir} is of "
                             f"{commit or 'no recorded commit'}, not {commits[name]}")


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
    if args.shard:
        over["run"]["shard"] = args.shard
    for s in args.set:
        k, _, v = s.partition("=")
        over["postgres"][k] = v
    suite = suites.get(args.suite)
    config = args.config or HERE / "configs" / suite.default_config
    cfg = config_mod.load(config, over, args.suite)
    if not args.am:
        raise SystemExit("at least one --am is required")

    _resolve_tools(cfg)
    if cfg["run"]["shard"]:
        from pbench import cshard
        print(cshard.to_text([cshard.apply(cfg, _corpus(cfg), cfg["run"]["shard"])]))

    # Each engine (and build) retains its result set; matched cells run as
    # randomized groups.
    ams = sorted(dict.fromkeys(args.am), key=lambda am: am != "heap")
    builds = pg_bins(args.pg_bin)
    commits = build_commits(args.build_commit, builds)
    if args.label and len(ams) * len(builds) > 1:
        raise SystemExit("--label names one result set; give one --am and one --pg-bin with it")
    if len(builds) > 1 and suite.layout != suites.STORAGE:
        raise SystemExit("builds are compared in storage suites only, as the S suite")
    if cfg["run"]["comparison_only"] and len(builds) < 2:
        raise SystemExit("a comparison_only configuration compares builds: give "
                         "--pg-bin base=DIR,candidate=DIR")
    check_build_commits(builds, commits)
    ok = True
    out_dirs = []
    runners = []
    for build, bin_dir in builds:
        for am in ams:
            label = args.label
            if not label:
                label = "B0" if am == "heap" else am
                if cfg["postgres"].get("full_page_writes") == "off" and suite.durable:
                    label += "-fpw-off"
                if build:
                    label = f"{build}-{label}"
            runner = Runner(suite=args.suite, cfg=cfg, table_am=am, label=label,
                            repo=REPO, bin_dir=bin_dir.resolve(),
                            work_dir=args.work_dir.resolve(), out_root=args.out.resolve(),
                            command="prolly_bench " + shlex.join(sys.argv[1:]),
                            keep_failed=args.keep_failed, build=build,
                            builds=tuple(name for name, _ in builds if name))
            runners.append(runner)
            out_dirs.append(runner.out_dir)
    from pbench.core.validation import validate_run
    all_results = run_many(runners, report_format=args.report_format)
    ok = all(bool(results) and not validate_run(runner.out_dir)
             for runner, results in zip(runners, all_results))
    if suite.layout == suites.STORAGE:
        from pbench.core.validation import compatible
        by = {(r.build, r.table_am): r.out_dir for r in runners}
        pairs = [(by[(b, "heap")], by[(b, am)], "engine")
                 for b, _ in builds for am in ams if am != "heap" and (b, "heap") in by]
        if len(builds) > 1:
            first = builds[0][0]
            pairs += [(by[(first, am)], by[(b, am)], "build")
                      for b, _ in builds[1:] for am in ams]
        for base, other, vary in pairs:
            problems = compatible(base, other, vary)
            for problem in problems:
                print(problem, file=sys.stderr)
            ok = ok and not problems
    if suite.layout == suites.CORRECTNESS and len(out_dirs) > 1:
        from pbench import ccompare, table
        print("\n" + ccompare.to_markdown([table.load_set(d) for d in out_dirs]))
        sets = [table.load_set(d) for d in out_dirs]
        base = next((s for s in sets if s.table_am == "heap"), None)
        from pbench.workloads.c_suite import WORKLOADS
        ok = ok and base is not None and all(not ccompare.gate(base, s,
            cfg["run"]["workloads"] or WORKLOADS)
            for s in sets if s is not base)
    return 0 if ok and not all_results.report_errors else 1


def _resolve_tools(cfg: dict) -> None:
    # Paths in [tools] are relative to the repository root.
    for k, v in list(cfg["tools"].items()):
        p = Path(v).expanduser()
        cfg["tools"][k] = str(p if p.is_absolute() else REPO / p)


def _corpus(cfg: dict) -> Path:
    from pbench.workloads.c_suite import corpus_dir
    return corpus_dir(cfg["tools"])


def _cshard(args) -> int:
    import json
    from pbench import cshard
    cfg = config_mod.load(args.config, {}, "C")
    _resolve_tools(cfg)
    shards = cshard.plan(_corpus(cfg), cfg, args.shards, cfg["run"]["workloads"])
    if args.json:
        print(json.dumps([{"shard": s.spec, "workloads": s.workloads, "bytes": s.bytes,
                           "files": s.files} for s in shards], indent=1))
    else:
        print(cshard.to_text(shards))
    return 0


def _list(_args) -> int:
    registry.load_all()
    for suite in suites.all_suites():
        print(f"{suite.letter} suite ({suite.title}) workloads:")
        for name, cls in sorted(registry.workloads.items(), key=lambda i: suites.workload_order(i[0])):
            if cls.suite == suite.letter:
                print(f"  {name:<4} {cls.title}")
    print("collectors: " + ", ".join(registry.collectors.names()))
    print("sinks:      " + ", ".join(registry.sinks.names()))
    print("clusters:   " + ", ".join(registry.clusters.names()))
    return 0


def _show_report(paths, passed, *, show_markdown=False) -> int:
    if show_markdown:
        for path in paths:
            if path.suffix == ".md":
                print(path.read_text())
    for path in paths:
        print(f"written: {path}")
    return int(not passed)


def _render_report(dirs, output, formats, *, baseline=None, notes=(), show_markdown=False) -> int:
    from pbench import presentation, report_files, reports
    sources = [reports.load(d) for d in dirs]
    if output is None:
        if len(sources) != 1:
            raise ValueError("multiple result sets require --output STEM")
        paths, passed = report_files.result_set(sources[0], formats)
    else:
        document = reports.build(sources, output.parent, baseline=baseline)
        document.notes[0:0] = notes
        paths = presentation.write(document, output, formats)
        passed = document.passed
    return _show_report(paths, passed, show_markdown=show_markdown)


def _report(args) -> int:
    from pbench import report_files
    if args.catalog:
        if args.dirs or args.manifest or args.output:
            raise ValueError("--catalog cannot be combined with inputs, --manifest or --output")
        return _show_report(report_files.catalog(args.catalog, args.format), True)
    if args.manifest:
        if args.dirs or args.output:
            raise ValueError("--manifest cannot be combined with inputs or --output")
        return _show_report(*report_files.invocation(args.manifest, formats=args.format))
    if not args.dirs:
        raise ValueError("give result directories, --manifest or --catalog")
    return _render_report(args.dirs, args.output, args.format)


def _compare(args) -> int:
    if args.format:
        if not args.output:
            raise ValueError("--format requires --output STEM")
        return _render_report([args.baseline, args.dir], args.output, args.format,
                              baseline=args.baseline)
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
    eng = table.load_set(args.dir)
    problems = ccompare.gate(table.load_set(args.baseline), eng,
                             args.workloads or ccompare.run_workloads(eng))
    for problem in problems:
        print(problem, file=sys.stderr)
    if not problems:
        print("C suite gate passed")
    return int(bool(problems))


def _creport(args) -> int:
    import json
    from pbench import creport
    report = creport.build(args.dirs, args.shards, args.base_baseline, corpus=args.corpus,
                           head=args.head, base=args.base, base_ref=args.base_ref,
                           run_url=args.run_url, wall_clock=args.wall_clock,
                           failed_jobs=args.failed_job, engines=args.engines)
    args.out.write_text(report.text)
    if args.json:
        args.json.write_text(json.dumps(report.summary, indent=1) + "\n")
    for problem in report.problems:
        print(problem, file=sys.stderr)
    print(f"C suite: {'PASS' if report.passed else 'FAIL'}; written: {args.out}")
    from pbench import finding_reports
    finding_reports.write(lambda: finding_reports.correctness(report.summary,
        {"base": args.base, "head": args.head, "base_ref": args.base_ref,
         "run_url": args.run_url, "wall_clock": args.wall_clock}, report.details),
        args.out, getattr(args, "report_output", None), args.dirs)
    return int(not report.passed)


def _cbaseline(args) -> int:
    import json
    from pbench import cbaseline, ccompare, table
    sets = [table.load_set(d) for d in args.dirs]
    commit = cbaseline.pin(args.pin)
    if args.action == "update":
        problems = cbaseline.update(sets, args.baseline, commit, ccompare.corpus_path(sets[0]))
        for problem in problems:
            print(problem, file=sys.stderr)
        if not problems:
            print(f"written: {args.baseline}")
        return int(bool(problems))
    check = cbaseline.check(sets, cbaseline.read(args.baseline), commit)
    if args.json:
        args.json.write_text(json.dumps(check.to_json(), indent=1) + "\n")
    for message in check.messages():
        print(message, file=sys.stderr)
    print(check.summary())
    return int(not check.ok)


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
    from pbench import discovery
    dirs = args.dirs or discovery.latest(args.results, args.suite)
    if not dirs:
        raise ValueError(f"no {args.suite} result sets under {args.results}")
    suite = discovery.header(dirs[0]).get("suite", args.suite)
    out = args.output or args.results / f"summary-{suite}"
    notes = [] if args.dirs else [
        "Selection: latest result set per label; these runs may belong to different invocations."]
    return _render_report(dirs, out, args.format, notes=notes, show_markdown=True)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="prolly_bench", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("run", help="run a suite")
    r.add_argument("suite", choices=suites.letters())
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
    r.add_argument("--pg-bin", default=str(REPO / "build-rel" / "inst" / "bin"),
                   help="the server's bin directory, or builds to compare, base first: "
                        "base=DIR,candidate=DIR (S suite)")
    r.add_argument("--build-commit", metavar="NAME=SHA,...",
                   help="the commit each build's provenance must name")
    r.add_argument("--work-dir", type=Path, default=REPO / "build-bench" / "work",
                   help="where clusters live: on the device under test")
    r.add_argument("--out", type=Path, default=REPO / "engine-results")
    r.add_argument("--no-fio", action="store_true", help="skip the fio device baseline")
    r.add_argument("--shard", metavar="I/N",
                   help="C suite: run shard I of N of the corpus (see cshard)")
    r.add_argument("--keep-failed", action="store_true",
                   help="keep the cluster of a cell that errors, for debugging")
    r.add_argument("--report-format", choices=["html", "markdown", "both", "none"], default="both",
                   help="derived report formats (default both; none keeps only execution evidence)")
    r.set_defaults(fn=_run)

    sub.add_parser("list", help="list workloads, collectors and sinks").set_defaults(fn=_list)

    rp = sub.add_parser("report", help="regenerate HTML and Markdown from saved evidence")
    rp.add_argument("dirs", type=Path, nargs="*")
    rp.add_argument("--format", choices=["html", "markdown", "both"], default="both")
    rp.add_argument("-o", "--output", type=Path, help="output stem; required for multiple inputs")
    rp.add_argument("--manifest", type=Path, help="an invocation's report.json")
    rp.add_argument("--catalog", type=Path, help="rebuild a result root's index")
    rp.set_defaults(fn=_report)

    c = sub.add_parser("compare", help="S results normalized to a baseline")
    c.add_argument("baseline", type=Path)
    c.add_argument("dir", type=Path)
    c.add_argument("-o", "--output", type=Path)
    c.add_argument("--format", choices=["html", "markdown", "both"],
                   help="write selected formats; --output is a stem in this mode")
    c.set_defaults(fn=_compare)

    t = sub.add_parser("table", help="engines side by side: a table per metric")
    t.add_argument("dirs", type=Path, nargs="*",
                   help="result sets (default: the latest per label)")
    t.add_argument("--suite", choices=suites.letters(), default="S")
    t.add_argument("--results", type=Path, default=REPO / "engine-results")
    t.add_argument("-o", "--output", type=Path,
                   help="output path without extension (default RESULTS/summary-SUITE)")
    t.add_argument("--format", choices=["html", "markdown", "both"], default="both")
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
    gate.add_argument("--workloads", type=_csv(str),
                      help="default: the workloads the run had (a shard's, or C1-C4)")
    gate.set_defaults(fn=_cgate)

    sh = sub.add_parser("cshard", help="C suite: the files each of N shards runs")
    sh.add_argument("--shards", type=int, required=True)
    sh.add_argument("--config", type=Path, default=HERE / "configs" / "c_full_ci.toml")
    sh.add_argument("--json", action="store_true")
    sh.set_defaults(fn=_cshard)

    from pbench import cbaseline
    hb = sub.add_parser("cbaseline", help="C suite: heap's results against the checked-in baseline")
    hb.add_argument("action", choices=["check", "update"])
    hb.add_argument("dirs", nargs="+", type=Path, help="heap result sets: a run or its shards")
    hb.add_argument("--baseline", type=Path, default=cbaseline.BASELINE)
    hb.add_argument("--pin", type=Path, default=cbaseline.PIN,
                    help="the file holding the corpus commit")
    hb.add_argument("--json", type=Path, help="check: also write the findings here")
    hb.set_defaults(fn=_cbaseline)

    cr = sub.add_parser("creport", help="C suite: the pull-request comment from all shards")
    cr.add_argument("dirs", nargs="+", type=Path, help="every shard's result sets")
    cr.add_argument("--shards", type=int, required=True, help="how many shards there should be")
    cr.add_argument("--base-baseline", type=Path, required=True,
                    help="the heap baseline at the base commit, for the change against it")
    cr.add_argument("--out", type=Path, required=True, help="the comment, Markdown")
    cr.add_argument("--json", type=Path, help="also a summary, JSON")
    cr.add_argument("--report-output", type=Path, help="full HTML/Markdown output stem (default OUT-stem-full)")
    cr.add_argument("--corpus", type=Path, help="the corpus's test/ directory "
                    "(default: as the result sets recorded it)")
    cr.add_argument("--engines", type=_csv(str),
                    help="the engines besides heap every shard must have (default: those found)")
    cr.add_argument("--head", default="", help="the pull request's head commit")
    cr.add_argument("--base", default="", help="the base commit")
    cr.add_argument("--base-ref", default="master")
    cr.add_argument("--run-url", default="", help="the CI run, for the artifacts")
    cr.add_argument("--wall-clock", default="", help="e.g. '14 min'")
    cr.add_argument("--failed-job", action="append", default=[],
                    help="a CI job that failed (repeatable); fails the report")
    cr.set_defaults(fn=_creport)

    evidence = sub.add_parser("gate", help="validate complete, intact execution evidence and optional baseline compatibility")
    evidence.add_argument("dirs", nargs="+", type=Path)
    evidence.add_argument("--baseline", type=Path)
    evidence.set_defaults(fn=_gate)

    from pbench import scounters
    sc = sub.add_parser("scounters", help="S counter runs: counters two runs of one build disagree on")
    sc.add_argument("one", type=Path)
    sc.add_argument("two", type=Path)
    sc.add_argument("--instructions-tolerance", type=float, default=0.01,
                    help="relative difference instruction counts may have (default 0.01)")
    sc.set_defaults(fn=scounters.run)

    from pbench import scalibrate
    cal = sub.add_parser("scalibrate", help="S suite gate: noise of A/A counter and timing runs")
    cal.add_argument("--counters", type=Path, nargs="*", default=[],
                     help="counter runs of one build (configs/s_counters.toml)")
    cal.add_argument("--timing", type=Path, nargs="*", default=[],
                     help="timing runs of one build against itself (configs/s_pr.toml)")
    cal.add_argument("--thresholds", type=Path, help="the gate's settings (configs/s_gate.toml)")
    cal.add_argument("--out", type=Path, help="Markdown report")
    cal.add_argument("--json", type=Path)
    cal.add_argument("--report-output", type=Path, help="full HTML/Markdown output stem (default OUT-stem-full)")
    cal.set_defaults(fn=scalibrate.run)

    from pbench import sgate
    sg = sub.add_parser("sgate", help="S suite gate: a pull request's counters and timing against its base's")
    sg.add_argument("layer", choices=["counters", "timing"])
    sg.add_argument("runs", type=Path, nargs="+",
                    help="counters: BASE BASE2 CANDIDATE CANDIDATE2; timing: RUN [RETRY ...]")
    sg.add_argument("--accept", type=Path, help="perf_accept.toml, as the pull request has it")
    sg.add_argument("--accept-base", type=Path,
                    help="perf_accept.toml as the base has it: only entries the pull request adds count")
    sg.add_argument("--thresholds", type=Path, help="the gate's settings (configs/s_gate.toml)")
    sg.add_argument("--report-only", action="store_true", help="timing: report, never fail")
    sg.add_argument("--json", type=Path, help="the findings, for sreport")
    sg.set_defaults(fn=sgate.run)

    from pbench import sreport
    sr = sub.add_parser("sreport", help="S suite gate: the pull-request comment")
    sr.add_argument("--counters", type=Path, action="append", default=[],
                    help="sgate counters --json output (repeatable: one per group of workloads)")
    sr.add_argument("--timing", type=Path, help="sgate timing --json output")
    sr.add_argument("--head", required=True)
    sr.add_argument("--base", required=True)
    sr.add_argument("--runner", default="ubuntu-24.04")
    sr.add_argument("--run-url")
    sr.add_argument("--failed-job", action="append", default=[], metavar="NAME: RESULT")
    sr.add_argument("--reproduce", help="a command that reproduces the run")
    sr.add_argument("--out", type=Path, required=True)
    sr.add_argument("--report-output", type=Path, help="full HTML/Markdown output stem (default OUT-stem-full)")
    sr.add_argument("--results", type=Path, action="append", default=[], help="retained result roots to link (repeatable)")
    sr.add_argument("--nightly", action="store_true",
                    help="a night against the night before: timing only")
    sr.set_defaults(fn=sreport.run)

    from pbench import shistory
    sh = sub.add_parser("shistory", help="S suite nightly: a night's summary for perf-history")
    sh.add_argument("run", type=Path, help="the nightly's timing run")
    sh.add_argument("--timing", type=Path, help="sgate timing --json output")
    sh.add_argument("--instructions", type=Path, help="a cachegrind run (s_cachegrind.toml)")
    sh.add_argument("--date", required=True)
    sh.add_argument("--out", type=Path, required=True)
    sh.set_defaults(fn=shistory.run)

    from pbench import complexity
    growth = sub.add_parser("complexity", help="gate node-work growth at fixed workload variant across dataset sizes")
    growth.add_argument("dir", type=Path)
    growth.add_argument("--workload", choices=["V1", "V2", "V3", "V4"], default="V2")
    growth.add_argument("--operation", default="dolt_commit")
    growth.add_argument("--counter", choices=["nodes_read", "nodes_written"], default="nodes_read")
    growth.add_argument("--max-exponent", type=float, required=True)
    growth.set_defaults(fn=complexity.run)

    from pbench import vgrowth
    vg = sub.add_parser("vgrowth", help="V suite: per-commit work growing with the number of commits before it")
    vg.add_argument("dir", type=Path, help="a result set of configs/v_counters.toml")
    vg.add_argument("--bound", type=float, default=vgrowth.DEFAULT_BOUND)
    vg.add_argument("--accept", type=Path, help="known growth (configs/v_growth_accept.toml)")
    vg.add_argument("--json", type=Path)
    vg.set_defaults(fn=vgrowth.run)

    from pbench import vshape
    vs = sub.add_parser("vshape", help="V suite: each version-control operation's work as N and Δ grow, "
                                       "against its target")
    vs.add_argument("dir", type=Path, help="a result set of configs/v_shape.toml")
    vs.add_argument("--known", type=Path, help="known exceptions (configs/v_shape_known.toml)")
    vs.add_argument("--json", type=Path)
    vs.add_argument("--markdown", type=Path, help="every finding, as a table")
    vs.set_defaults(fn=vshape.run)

    from pbench import vclean
    vcl = sub.add_parser("vclean", help="V suite: the same queries before and after storage "
                                        "cleanup, their latencies compared")
    vcl.add_argument("dir", type=Path, help="a result set of configs/v_cleanup.toml")
    vcl.add_argument("--tolerance", type=float, default=vclean.TOLERANCE,
                     help="a query's paired phase 2 over phase 1 ratio above this is slower")
    vcl.add_argument("--floor-ms", type=float, default=vclean.FLOOR_MS,
                     help="and only by more than this many milliseconds")
    vcl.add_argument("--gate-latency", action="store_true",
                     help="fail on a slower query (digests always gate: the run fails)")
    vcl.add_argument("--json", type=Path)
    vcl.add_argument("--markdown", type=Path, help="every query, as a table")
    vcl.set_defaults(fn=vclean.run)

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

    vcfuzz = sub.add_parser("vcfuzz", help="seeded version-control histories checked against a model")
    vcfuzz.add_argument("--pg-bin", type=Path, required=True)
    vcfuzz.add_argument("--out", type=Path, required=True, help="new artifact directory")
    vcfuzz.add_argument("--runner", type=Path, default=REPO / "build-bench/differential/vc-fuzz")
    vcfuzz.add_argument("--seed", type=int, default=1)
    vcfuzz.add_argument("--seeds", type=int, default=10)
    vcfuzz.add_argument("--steps", type=int, default=80)
    vcfuzz.add_argument("--sessions", type=int, default=0, help="sessions per history; 0 varies them by seed")
    vcfuzz.add_argument("--reduce", type=int, default=0, help="bounded reduction attempts on failure")
    vcfuzz.add_argument("--replay", help="saved history.json or reduced.json files (a glob)")
    vcfuzz.add_argument("--self-test", action="store_true", help="also prove the checks reject known wrong engines")
    vcfuzz.set_defaults(fn=differential.run_vc)

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
