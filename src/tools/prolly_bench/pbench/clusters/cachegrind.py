"""A local cluster whose server runs under Cachegrind, which counts the
instructions every server process executes, with no hardware counters.

Each process the postmaster forks writes its own count when it exits
(cachegrind.out.<pid> under the cluster's directory), so the measured
clients' backends, which start and end inside the window, are counted on
their own; the cachegrind collector sums those.  The server runs tens of
times slower, so this is for a reduced workload, nightly."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

from pbench.clusters.local import LocalCluster
from pbench.core import registry
from pbench.core.interfaces import Unavailable


@registry.clusters.register("cachegrind")
class CachegrindCluster(LocalCluster):
    name = "cachegrind"

    @property
    def counts_dir(self) -> Path:
        return self.base / "cachegrind"

    def start(self) -> None:
        if shutil.which("valgrind") is None:
            raise Unavailable("valgrind is not installed")
        self.counts_dir.mkdir(parents=True, exist_ok=True)
        log = open(self.log_file, "a")
        self._server = subprocess.Popen(
            ["valgrind", "--tool=cachegrind", "--cache-sim=no", "--trace-children=yes",
             f"--cachegrind-out-file={self.counts_dir}/cachegrind.out.%p",
             f"--log-file={self.counts_dir}/valgrind.%p.log",
             self.bin("postgres"), "-D", str(self.data_dir)],
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        log.close()
        self._running = True
        try:
            self.wait_ready(timeout=600)
        except RuntimeError:
            raise RuntimeError(f"server under valgrind did not start:\n{self.log_since(0)[-3000:]}")

    def stop(self, mode: str = "fast") -> None:
        super().stop(mode)
        server = getattr(self, "_server", None)
        if server is not None:
            server.wait(timeout=600)
