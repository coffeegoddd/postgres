"""Seeded SQL correctness runs on an isolated, durable local cluster."""

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
        "revision": revision, "dirty": bool(dirty), "engine": args.am, "settings": settings,
        "pg_bin": str(args.pg_bin.resolve()),
        "postgres_version": subprocess.check_output(
            [str(args.pg_bin.resolve() / "postgres"), "--version"], text=True).strip(),
        "runner_sha256": hashlib.sha256(runner.read_bytes()).hexdigest(),
        "command": "prolly_bench " + shlex.join(sys.argv[1:]),
    }, indent=2) + "\n")
    with tempfile.TemporaryDirectory(prefix="engine-diff-") as work:
        cluster = LocalCluster(args.pg_bin.resolve(), Path(work), free_port())
        try:
            cluster.create(settings)
            cluster.start()
            command = [str(runner), "-dsn", cluster.conninfo(), "-am", args.am,
                       "-seed", str(args.seed), "-seeds", str(args.seeds),
                       "-steps", str(args.steps), "-reduce", str(args.reduce),
                       "-out", str(out / "cases")]
            if args.replay:
                command += ["-replay", str(args.replay.resolve())]
            if args.self_test:
                command += ["-self-test"]
            with (out / "runner.log").open("w") as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            print((out / "runner.log").read_text(), end="")
            return result.returncode
        finally:
            if cluster.log_file.exists():
                shutil.copy2(cluster.log_file, out / "server.log")
            cluster.destroy()
