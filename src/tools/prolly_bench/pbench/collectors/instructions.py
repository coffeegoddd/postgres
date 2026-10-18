"""Instructions the server retired during the window (perf stat on the
postmaster and the processes it forks), where the machine has hardware
counters: user instructions, and kernel ones where perf_event_paranoid
allows (<= 1; src/tools/prolly_bench/system/system_prepare.sh sets -1).

Many virtual machines expose no hardware counters: perf then reports the
event "not supported", and the collector is unavailable rather than wrong.
The cachegrind cluster and collector count instructions without hardware,
far more slowly."""

from __future__ import annotations

import os
import shutil
import signal
import subprocess
import time
from pathlib import Path
from typing import Optional

from pbench.core import registry
from pbench.core.interfaces import Collector, Unavailable
from pbench.core.stats import per

USER, KERNEL = "instructions:u", "instructions:k"
EVENTS = (USER, KERNEL)


def parse_perf_stat(text: str) -> dict[str, Optional[float]]:
    """{event: count} from perf stat -x, output; None for an event perf
    couldn't count ("<not supported>", "<not counted>")."""
    out: dict[str, Optional[float]] = {}
    for line in text.splitlines():
        fields = line.split(",")
        if len(fields) < 3 or line.startswith("#"):
            continue
        value, event = fields[0], fields[2]
        if event not in EVENTS:
            continue
        try:
            out[event] = float(value)
        except ValueError:
            out[event] = None
    return out


def probe() -> tuple[list[str], str]:
    """The instruction events this machine counts, and what perf said."""
    if shutil.which("perf") is None:
        return [], "perf is not installed"
    r = subprocess.run(["perf", "stat", "-x", ",", "-e", ",".join(EVENTS), "--", "true"],
                       capture_output=True, text=True, timeout=60)
    counts = parse_perf_stat(r.stderr)
    if r.returncode != 0 or set(counts) != set(EVENTS):
        return [], f"perf stat failed: {r.stderr.strip()[-300:]}"
    said = "; ".join(line for line in r.stderr.splitlines() if "instructions" in line)
    events = [e for e in EVENTS if counts[e] is not None]
    if USER not in events:
        return [], "no hardware instruction counter: " + said
    return events, said


def postmaster_pid(data_dir: Path) -> int:
    return int((data_dir / "postmaster.pid").read_text().split("\n", 1)[0])


@registry.collectors.register("instructions")
class InstructionsCollector(Collector):
    name = "instructions"

    def check(self, ctx):
        self.events, reason = probe()
        if not self.events:
            raise Unavailable(reason)

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
                if self.proc.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("perf stat did not start: "
                                       + self.proc.stderr.read()[-1000:])
                time.sleep(0.01)
        with os.fdopen(fd, "w") as c, open(ack) as a:
            c.write("enable\n")
            c.flush()
            if a.readline().strip() != "ack":
                raise RuntimeError("perf stat did not start counting")

    def stop(self, ctx):
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
