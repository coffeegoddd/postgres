"""A cluster on this machine, managed with initdb and pg_ctl."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Optional
from urllib.parse import quote

from pbench.core import registry
from pbench.core import process
from pbench.core.interfaces import Cluster, SqlResult
from pbench.core.sqlerr import raise_for_sql_error, sqlstate_of


def pgoptions(options: Optional[dict[str, str]]) -> str:
    return " ".join(f"-c {k}={v}" for k, v in (options or {}).items())


@registry.clusters.register("local")
class LocalCluster(Cluster):
    name = "local"

    def __init__(self, bin_dir: Path, work_dir: Path, port: int,
                 instance: str = "main") -> None:
        self.bin_dir = Path(bin_dir)
        self.base = Path(work_dir) / instance
        self.data_dir = self.base / "data"
        self.log_file = self.base / "server.log"
        self.port = port
        # Unix socket paths are limited to ~107 bytes, so keep them short.
        self.sock_dir = Path(tempfile.mkdtemp(prefix="pbench-"))
        self._running = False

    # -- lifecycle --------------------------------------------------------

    def bin(self, program: str) -> str:
        return str(self.bin_dir / program)

    def _run(self, args: list[str], **kw) -> subprocess.CompletedProcess:
        return process.run(args, capture_output=True, text=True, **kw)

    def create(self, settings: dict[str, str]) -> None:
        if self._running:
            self.stop("immediate")
        if self.data_dir.exists():
            shutil.rmtree(self.data_dir)
        self.base.mkdir(parents=True, exist_ok=True)
        if self.log_file.exists():
            self.log_file.unlink()
        r = self._run([self.bin("initdb"), "-D", str(self.data_dir), "-U", "postgres",
                       "-A", "trust", "-E", "UTF8", "--locale=C", "--no-instructions"])
        if r.returncode != 0:
            raise RuntimeError(f"initdb failed: {r.stderr}")
        lines = ["", "# --- prolly_bench ---"]
        for k, v in {**settings, "port": str(self.port),
                     "unix_socket_directories": f"'{self.sock_dir}'"}.items():
            lines.append(f"{k} = {v}")
        with open(self.data_dir / "postgresql.conf", "a") as f:
            f.write("\n".join(lines) + "\n")

    def start(self) -> None:
        r = self._run([self.bin("pg_ctl"), "-D", str(self.data_dir), "-l",
                       str(self.log_file), "-w", "-t", "3600", "start"])
        if r.returncode != 0:
            raise RuntimeError(f"pg_ctl start failed: {r.stdout}{r.stderr}\n"
                               f"{self.log_since(0)[-3000:]}")
        self._running = True

    def stop(self, mode: str = "fast") -> None:
        if not self.data_dir.exists():
            return
        r = self._run([self.bin("pg_ctl"), "-D", str(self.data_dir), "-m", mode,
                       "-w", "-t", "3600", "stop"])
        if r.returncode != 0 and "Is server running?" not in r.stderr:
            raise RuntimeError(f"pg_ctl stop failed: {r.stdout}{r.stderr}")
        self._running = False

    def wait_ready(self, timeout: float = 600.0) -> float:
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            r = self._run([self.bin("pg_isready"), "-q", "-h", str(self.sock_dir),
                           "-p", str(self.port)])
            if r.returncode == 0:
                return time.monotonic() - t0
            time.sleep(0.01)
        raise RuntimeError("server did not become ready")

    def destroy(self) -> None:
        try:
            self.stop("immediate")
        finally:
            # (a failed stop still frees the cell's disk: each cell has its own)
            if self.base.exists():
                shutil.rmtree(self.base, ignore_errors=True)
            shutil.rmtree(self.sock_dir, ignore_errors=True)

    # -- clients ----------------------------------------------------------

    def client_env(self, options: Optional[dict[str, str]] = None) -> dict[str, str]:
        env = dict(os.environ)
        env.update(PGHOST=str(self.sock_dir), PGPORT=str(self.port), PGUSER="postgres",
                   PGOPTIONS=pgoptions(options))
        env.pop("PGDATABASE", None)
        return env

    def jdbc_url(self, dbname: str, options: Optional[dict[str, str]] = None) -> str:
        url = f"jdbc:postgresql://localhost:{self.port}/{dbname}?sslmode=disable"
        if options:
            url += "&options=" + quote(pgoptions(options))
        return url

    def libpq_url(self, dbname: str) -> str:
        return (f"postgresql:///{dbname}?host={quote(str(self.sock_dir), safe='')}"
                f"&port={self.port}&user=postgres")

    def conninfo(self) -> str:
        return f"host={self.sock_dir} port={self.port} user=postgres"

    def sql(self, sql: str, *, dbname: str = "postgres",
            options: Optional[dict[str, str]] = None,
            check: bool = True) -> SqlResult:
        r = process.run(
            [self.bin("psql"), "-X", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=1",
             "-v", "VERBOSITY=verbose", "-d", dbname, "-f", "-"],
            input=sql, capture_output=True, text=True, env=self.client_env(options))
        res = SqlResult(ok=r.returncode == 0, stdout=r.stdout, stderr=r.stderr,
                        sqlstate=sqlstate_of(r.stderr))
        if check:
            raise_for_sql_error(res, sql)
        return res

    def scalar(self, sql: str, *, dbname: str = "postgres") -> str:
        return self.sql(sql, dbname=dbname).stdout.strip()

    # -- server log -------------------------------------------------------

    def log_position(self) -> int:
        return self.log_file.stat().st_size if self.log_file.exists() else 0

    def log_since(self, position: int) -> str:
        if not self.log_file.exists():
            return ""
        with open(self.log_file, "r", errors="replace") as f:
            f.seek(position)
            return f.read()
