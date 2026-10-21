"""Linux server-tree memory sampling; PSS avoids counting shared pages twice."""
import os
from pathlib import Path
import threading
import time


def descendants(pid):
    found, todo = set(), [pid]
    while todo:
        p = todo.pop()
        if p in found:
            continue
        found.add(p)
        # Children can be created by any thread (especially Go).
        for task in Path(f"/proc/{p}/task").glob("*/children"):
            try:
                todo.extend(map(int, task.read_text().split()))
            except FileNotFoundError:
                pass
    return found


def snapshot(pid):
    totals = dict(rss_bytes=0, pss_bytes=0, private_bytes=0, swap_bytes=0,
                  minor_faults=0, major_faults=0, cpu_seconds=0,
                  read_bytes=0, write_bytes=0)
    seen = []
    for p in descendants(pid):
        try:
            stat = Path(f"/proc/{p}/stat").read_text().rsplit(")", 1)[1].split()
            smaps = Path(f"/proc/{p}/smaps_rollup").read_text()
            io = Path(f"/proc/{p}/io").read_text()
        except (FileNotFoundError, ProcessLookupError):
            continue
        # Permission errors deliberately fail measurement, rather than invent zeros.
        mem = {s.split(':')[0]: int(s.split()[1])*1024
               for s in smaps.splitlines() if s.split()[0].endswith(':')}
        totals["rss_bytes"] += mem["Rss"]
        totals["pss_bytes"] += mem["Pss"]
        totals["private_bytes"] += mem.get("Private_Clean", 0) + mem.get("Private_Dirty", 0)
        totals["swap_bytes"] += mem.get("Swap", 0)
        totals["minor_faults"] += int(stat[7])
        totals["major_faults"] += int(stat[9])
        totals["cpu_seconds"] += (int(stat[11]) + int(stat[12])) / os.sysconf("SC_CLK_TCK")
        counters = dict(line.split(': ') for line in io.splitlines())
        for k in ("read_bytes", "write_bytes"):
            totals[k] += int(counters[k])
        seen.append(p)
    if pid not in seen:
        raise RuntimeError(f"server {pid} disappeared while sampling")
    return {**totals, "pids": sorted(seen), "monotonic_ns": time.monotonic_ns()}


class Sampler:
    def __init__(self, pid, interval_ms):
        self.pid = pid
        self.interval = interval_ms / 1000
        self.done = threading.Event()
        self.samples = []
        self.error = None

    def start(self):
        self.samples.append(snapshot(self.pid))
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        try:
            while not self.done.wait(self.interval):
                self.samples.append(snapshot(self.pid))
        except Exception as e:
            self.error = e

    def finish(self, elapsed_ns):
        self.done.set()
        self.thread.join()
        if self.error:
            raise self.error
        periodic = len(self.samples)-1
        self.samples.append(snapshot(self.pid))
        first, last = self.samples[0], self.samples[-1]
        result = {"interval_ms": self.interval*1000, "periodic_samples": periodic,
                  "peak_is_lower_bound": True,
                  "short_operation": elapsed_ns < self.interval*1e9 or periodic == 0,
                  "process_set_changed": first["pids"] != last["pids"],
                  "samples": self.samples}
        for field in ("rss_bytes", "pss_bytes", "private_bytes", "swap_bytes"):
            result[field+"_baseline"] = first[field]
            result[field+"_peak"] = max(s[field] for s in self.samples)
            result[field+"_peak_delta"] = result[field+"_peak"] - first[field]
            result[field+"_retained_delta"] = last[field] - first[field]
        for field in ("minor_faults", "major_faults", "cpu_seconds", "read_bytes", "write_bytes"):
            result[field+"_delta"] = (last[field] - first[field]
                                      if not result["process_set_changed"] else None)
        return result
