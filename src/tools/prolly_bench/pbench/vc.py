"""Run version-control scenarios on a disposable, durable PostgreSQL cluster."""

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
    runner = args.runner.resolve()
    if not runner.is_file():
        raise SystemExit(f"missing {runner}; run build_vc.sh first")
    cases = args.cases.resolve()
    if not cases.exists():
        raise SystemExit(f"missing scenario path: {cases}")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    settings = {
        "fsync": "on", "synchronous_commit": "on", "full_page_writes": "on",
        "autovacuum": "off", "listen_addresses": "''",
        "log_line_prefix": "'%m [%p] %e '",
    }
    repo = Path(__file__).resolve().parents[4]
    (out / "environment.json").write_text(json.dumps({
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
        "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=repo, text=True)),
        "engines": args.am, "settings": settings,
        "postgres_version": subprocess.check_output(
            [str(args.pg_bin.resolve() / "postgres"), "--version"], text=True).strip(),
        "pg_bin": str(args.pg_bin.resolve()),
        "runner_sha256": hashlib.sha256(runner.read_bytes()).hexdigest(),
        "cases": str(cases), "command": "prolly_bench " + shlex.join(sys.argv[1:]),
    }, indent=2) + "\n")
    with tempfile.TemporaryDirectory(prefix="engine-vc-") as work:
        cluster = LocalCluster(args.pg_bin.resolve(), Path(work), free_port())
        try:
            cluster.create(settings)
            cluster.start()
            command = [str(runner), "-dsn", cluster.conninfo(), "-am", args.am,
                       "-cases", str(cases), "-out", str(out / "results")]
            if args.self_test:
                command += ["-self-test"]
            with (out / "runner.log").open("w") as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            print((out / "runner.log").read_text(), end="")
            return result.returncode
        finally:
            try:
                cluster.stop("fast")
            finally:
                if cluster.log_file.exists():
                    shutil.copy2(cluster.log_file, out / "server.log")
                cluster.destroy()
