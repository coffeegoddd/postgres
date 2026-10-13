"""CPU profile of the whole machine during the window (perf record -a -g),
with the share of samples in hashing functions.

Needs perf and kernel.perf_event_paranoid <= -1
(src/tools/prolly_bench/system/system_prepare.sh sets it)."""

from __future__ import annotations

import re
import shutil
import signal
import subprocess

from pbench.core import registry
from pbench.core.interfaces import Collector, Unavailable

# Symbols counted as hashing (content hashes, boundary hashes).
DEFAULT_HASH_SYMBOLS = r"sha256|SHA256|pg_cryptohash|blake|xxh|hash_bytes"


@registry.collectors.register("perf")
class PerfCollector(Collector):
    name = "perf"

    def check(self, ctx):
        if shutil.which("perf") is None:
            raise Unavailable("perf is not installed")
        with open("/proc/sys/kernel/perf_event_paranoid") as f:
            level = int(f.read())
        if level > -1:
            raise Unavailable(f"kernel.perf_event_paranoid = {level}; run "
                              "src/tools/prolly_bench/system/system_prepare.sh")

    def start(self, ctx):
        self.data = ctx.scratch_dir / "perf.data"   # RAM, not the device under test
        self.proc = subprocess.Popen(
            ["perf", "record", "-a", "-g", "-F", str(ctx.params.get("perf_frequency", 99)),
             "-o", str(self.data)],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)

    def stop(self, ctx):
        self.proc.send_signal(signal.SIGINT)
        _, err = self.proc.communicate(timeout=300)
        (ctx.raw_dir / "perf-record.log").write_text(err)

    def results(self, ctx, outcome):
        report = subprocess.run(
            ["perf", "report", "-i", str(self.data), "--stdio", "--no-children",
             "--sort", "dso,symbol", "--percent-limit", "0.1"],
            capture_output=True, text=True).stdout
        (ctx.raw_dir / "perf-report.txt").write_text(report)
        pattern = re.compile(ctx.params.get("hash_symbols", DEFAULT_HASH_SYMBOLS))
        samples = re.search(r"# Samples: (\S+)", report)
        hash_share = 0.0
        for line in report.splitlines():
            m = re.match(r"\s+([\d.]+)%\s+(\S+)\s+\[.\]\s+(\S+)", line)
            if m and pattern.search(m.group(3)):
                hash_share += float(m.group(1))
        return {"perf_samples": _count(samples.group(1)) if samples else None,
                "hash_cpu_percent": hash_share}


def _count(s: str) -> float:
    mult = {"K": 1e3, "M": 1e6, "G": 1e9}
    return float(s[:-1]) * mult[s[-1]] if s[-1] in mult else float(s)
