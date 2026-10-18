"""Instructions the server retired during the window (perf stat on the
postmaster and the processes it forks), where the machine has hardware
counters: user instructions, and kernel ones where perf_event_paranoid
allows (<= 1; src/tools/prolly_bench/system/system_prepare.sh sets -1).

Many virtual machines expose no hardware counters (GitHub's hosted runners
don't): perf then reports the event "not supported", and the collector is
unavailable rather than wrong.  The cachegrind cluster and collector count
instructions without hardware, far more slowly."""

from __future__ import annotations

import os
import select
import shutil
import signal
import subprocess
import time
from pathlib import Path
from typing import Optional

from pbench.collectors._snapshot import settled, wait_for_clients
from pbench.core import registry
from pbench.core.interfaces import Collector, Unavailable
from pbench.core.stats import per

USER, KERNEL = "instructions:u", "instructions:k"
EVENTS = (USER, KERNEL)


def _event(name: str) -> Optional[str]:
    """USER or KERNEL for perf's name of an instruction event: plain, or a
    hybrid CPU's per-PMU one ("cpu_core/instructions:u/")."""
    name = name.strip("/")
    base = name.rsplit("/", 1)[-1]
    return base if base in EVENTS else None


def parse_perf_stat(text: str) -> dict[str, Optional[float]]:
    """{event: count} from perf stat -x, output, a hybrid CPU's PMUs summed;
    None for an event no PMU could count ("<not supported>")."""
    out: dict[str, Optional[float]] = {}
    for line in text.splitlines():
        fields = line.split(",")
        if len(fields) < 3 or line.startswith("#"):
            continue
        event = _event(fields[2])
        if event is None:
            continue
        try:
            value = float(fields[0])
        except ValueError:
            # (one PMU of a hybrid CPU may count nothing while another does)
            value = 0.0 if fields[0] == "<not counted>" else None
        if value is None:
            out.setdefault(event, None)
        else:
            out[event] = (out.get(event) or 0.0) + value
    return out


def probe() -> tuple[list[str], str]:
    """The instruction events this machine counts, each tried on its own,
    and what perf said."""
    if shutil.which("perf") is None:
        return [], "perf is not installed"
    events, said = [], []
    for e in EVENTS:
        r = subprocess.run(["perf", "stat", "-x", ",", "-e", e, "--", "true"],
                           capture_output=True, text=True, timeout=60)
        count = parse_perf_stat(r.stderr).get(e)
        said.append(f"{e}: " + (str(count) if r.returncode == 0 and count is not None
                                else "not counted: " + " ".join(r.stderr.split())[-200:]))
        if r.returncode == 0 and count is not None:
            events.append(e)
    if USER not in events:
        return [], "no hardware instruction counter: " + "; ".join(said)
    return events, "; ".join(said)


def postmaster_pid(data_dir: Path) -> int:
    return int((data_dir / "postmaster.pid").read_text().split("\n", 1)[0])


@registry.collectors.register("instructions")
class InstructionsCollector(Collector):
    name = "instructions"

    def check(self, ctx):
        self.events, reason = probe()
        if not self.events:
            raise Unavailable(reason)

    def _failed(self, why: str) -> RuntimeError:
        if self.proc.poll() is None:
            self.proc.kill()
        _, err = self.proc.communicate(timeout=60)
        return RuntimeError(f"perf stat {why}: {err[-1000:]}")

    def start(self, ctx):
        self.out = ctx.scratch_dir / "perf-stat.csv"
        ctl, ack = ctx.scratch_dir / "perf-ctl", ctx.scratch_dir / "perf-ack"
        os.mkfifo(ctl)
        os.mkfifo(ack)
        # Attached with counting off, then on once perf acknowledges, so
        # nothing the window holds starts before perf counts.  (Processes
        # forked after it attaches, the measured clients' backends, are
        # counted with the postmaster.)
        self.proc = subprocess.Popen(
            ["perf", "stat", "-x", ",", "-e", ",".join(self.events), "-o", str(self.out),
             "-D", "-1", f"--control=fifo:{ctl},{ack}",
             "-p", str(postmaster_pid(ctx.cluster.data_dir))],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        # (perf opens both fifos read-write; until it has, opening ctl to
        # write without a reader fails, which a perf that exited never is)
        deadline = time.monotonic() + 60
        while True:
            try:
                fd = os.open(ctl, os.O_WRONLY | os.O_NONBLOCK)
                break
            except OSError:
                if self.proc.poll() is not None:
                    raise self._failed("exited")
                if time.monotonic() > deadline:
                    raise self._failed("did not start in 60 s")
                time.sleep(0.01)
        afd = os.open(ack, os.O_RDONLY | os.O_NONBLOCK)
        try:
            os.write(fd, b"enable\n")
            ready, _, _ = select.select([afd], [], [], 60)
            if not ready or not os.read(afd, 64).startswith(b"ack"):
                raise self._failed("did not start counting")
        finally:
            os.close(fd)
            os.close(afd)

    def stop(self, ctx):
        # (the measured clients' backends end inside the window: counted to
        # their exit, as the settled counters are)
        if settled(ctx):
            wait_for_clients(ctx)
        self.proc.send_signal(signal.SIGINT)
        _, err = self.proc.communicate(timeout=120)
        text = self.out.read_text() if self.out.exists() else ""
        (ctx.raw_dir / "perf-stat.csv").write_text(text + err)
        if self.proc.returncode not in (0, -signal.SIGINT):
            raise RuntimeError(f"perf stat failed: {err[-1000:]}")
        self.counts = parse_perf_stat(text)

    def results(self, ctx, outcome):
        user, kernel = self.counts.get(USER), self.counts.get(KERNEL)
        if user is None:
            raise Unavailable("perf counted no instructions")
        out = {"instructions_user": user,
               "instructions_user_per_operation": per(user, outcome.transactions)}
        if kernel is not None:
            out["instructions_kernel"] = kernel
        return out
