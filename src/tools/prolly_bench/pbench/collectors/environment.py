"""Record host conditions throughout the window; never attribute them to the server."""
import threading
import time
from pathlib import Path

from pbench.core import registry
from pbench.core.interfaces import Collector
from pbench.core.validation import atomic_json


def snapshot():
    vm = dict(line.split() for line in Path("/proc/vmstat").read_text().splitlines())
    memory = {line.split(":")[0]: int(line.split()[1]) * 1024
              for line in Path("/proc/meminfo").read_text().splitlines() if ":" in line}
    governors = sorted({p.read_text().strip() for p in
                        Path("/sys/devices/system/cpu").glob("cpu[0-9]*/cpufreq/scaling_governor")})
    boost = Path("/sys/devices/system/cpu/cpufreq/boost")
    return {"monotonic": time.monotonic(), "governors": governors,
            "boost": boost.read_text().strip() if boost.exists() else "unknown",
            "swap_out_pages": int(vm["pswpout"]), "available_bytes": memory["MemAvailable"],
            "loadavg": Path("/proc/loadavg").read_text().strip()}


@registry.collectors.register("environment")
class EnvironmentCollector(Collector):
    name = "environment"

    def start(self, ctx):
        self.samples, self.errors = [snapshot()], []
        self.done = threading.Event()
        def sample():
            while not self.done.wait(1):
                try:
                    self.samples.append(snapshot())
                except Exception as e:
                    self.errors.append(str(e))
                    return
        self.thread = threading.Thread(target=sample, daemon=True)
        self.thread.start()

    def stop(self, ctx):
        self.done.set()
        self.thread.join(timeout=5)
        if self.thread.is_alive():
            raise RuntimeError("environment sampler did not stop")
        self.samples.append(snapshot())

    def results(self, ctx, outcome):
        atomic_json(ctx.raw_dir / "host-conditions.json", {"samples": self.samples, "errors": self.errors})
        if self.errors:
            raise RuntimeError("host monitoring failed: " + "; ".join(self.errors))
        swap = self.samples[-1]["swap_out_pages"] - self.samples[0]["swap_out_pages"]
        if ctx.params.get("quality") == "benchmark":
            if swap or any(s["governors"] != ["performance"] or s["boost"] == "1" for s in self.samples):
                raise RuntimeError("host swapped or CPU conditions changed during measurement")
        return {"host_swap_out_pages": swap,
                "host_min_available_bytes": min(s["available_bytes"] for s in self.samples)}
