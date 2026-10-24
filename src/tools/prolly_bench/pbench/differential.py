"""Seeded SQL correctness runs on an isolated, durable local cluster:
transaction histories compared with heap, and version-control histories
checked against a reference model."""

from __future__ import annotations

import hashlib
import json
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from pbench.clusters.local import LocalCluster
from pbench.core.runner import free_port


def run(args) -> int:
    if args.seeds < 1 or args.steps < 1 or args.reduce < 0:
        raise SystemExit("seeds and steps must be positive; reduce must be nonnegative")
    options = ["-am", args.am, "-seed", str(args.seed), "-seeds", str(args.seeds),
               "-steps", str(args.steps), "-reduce", str(args.reduce)]
    if args.replay:
        options += ["-replay", str(args.replay.resolve())]
    if args.self_test:
        options += ["-self-test"]
    options += postgis_options(args)
    return run_on_cluster(args, args.am, options)


def run_vc(args) -> int:
    if args.seeds < 1 or args.steps < 1 or args.reduce < 0 or args.sessions < 0:
        raise SystemExit("seeds and steps must be positive; reduce and sessions must be nonnegative")
    options = ["-seed", str(args.seed), "-seeds", str(args.seeds), "-steps", str(args.steps),
               "-sessions", str(args.sessions), "-reduce", str(args.reduce)]
    if args.replay:
        options += ["-replay", args.replay]
    if args.self_test:
        options += ["-self-test"]
    options += postgis_options(args)
    return run_on_cluster(args, "prolly", options)


def postgis_options(args) -> list[str]:
    """The runner's -postgis, once the server is seen to have PostGIS: a
    run without it would fail in its first setup statement."""
    if not getattr(args, "postgis", False):
        return []
    share = subprocess.check_output([str(args.pg_bin.resolve() / "pg_config"), "--sharedir"],
                                    text=True).strip()
    if not (Path(share) / "extension" / "postgis.control").is_file():
        raise SystemExit(f"--postgis: no PostGIS in {share}/extension; build it with build_postgis.py")
    return ["-postgis"]


def run_on_cluster(args, engine: str, options: list[str]) -> int:
    """Run the runner against a fresh cluster, recording its environment."""
    runner = args.runner.resolve()
    if not runner.is_file():
        raise SystemExit(f"missing {runner}; run build_diff.sh first")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    settings = {
        "fsync": "on", "synchronous_commit": "on", "full_page_writes": "on",
        "autovacuum": "off", "listen_addresses": "''",
        "log_line_prefix": "'%m [%p] %e '",
    }
    repo = Path(__file__).resolve().parents[4]
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo,
                              text=True, capture_output=True, check=True).stdout.strip()
    dirty = subprocess.check_output(["git", "status", "--porcelain"], cwd=repo, text=True)
    (out / "environment.json").write_text(json.dumps({
        "revision": revision, "dirty": bool(dirty), "engine": engine, "settings": settings,
        "postgis": "-postgis" in options,
        "pg_bin": str(args.pg_bin.resolve()),
        "postgres_version": subprocess.check_output(
            [str(args.pg_bin.resolve() / "postgres"), "--version"], text=True).strip(),
        "runner_sha256": hashlib.sha256(runner.read_bytes()).hexdigest(),
        "server_sha256": hashlib.sha256((args.pg_bin.resolve() / "postgres").read_bytes()).hexdigest(),
        "command": "prolly_bench " + shlex.join(sys.argv[1:]),
    }, indent=2) + "\n")
    with tempfile.TemporaryDirectory(prefix="engine-diff-") as work:
        cluster = LocalCluster(args.pg_bin.resolve(), Path(work), free_port())
        try:
            cluster.create(settings)
            cluster.start()
            command = [str(runner), "-dsn", cluster.conninfo(), *options, "-out", str(out / "cases")]
            with (out / "runner.log").open("w") as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            print((out / "runner.log").read_text(), end="")
            return result.returncode
        finally:
            if cluster.log_file.exists():
                shutil.copy2(cluster.log_file, out / "server.log")
            cluster.destroy()
