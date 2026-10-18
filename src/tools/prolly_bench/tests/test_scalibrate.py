"""Tests of the S suite gate's calibration (scalibrate), from fixtures.

Run: cd src/tools/prolly_bench && python3 -m unittest discover -s tests
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from evidence_helpers import seal
from pbench import scalibrate, scounters
from test_s_suite import build_set


def counter_run(root: Path, name: str, counters: dict, am="prolly") -> Path:
    """A counter run: a directory with one result set of one S1 cell."""
    d = root / name / f"x-S-{am}"
    d.mkdir(parents=True)
    (d / "run.json").write_text(json.dumps(
        {"suite": "S", "label": am, "table_am": am, "started": "2026-10-18"}))
    (d / "results.jsonl").write_text(json.dumps({
        "key": {"suite": "S", "workload": "S1", "size": "counters", "variant": "",
                "clients": 1, "rep": 1},
        "status": "ok", "metrics": counters, "denominator": 2000}) + "\n")
    seal(d)
    return root / name


class CalibrationTest(unittest.TestCase):
    def test_counter_noise(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            runs = [counter_run(root, f"r{i}", {"wal_bytes": 1000.0, "io_extends": 40.0 + (i % 2) * 20,
                                                "instructions_user": 1e8 * (1 + i / 10000)})
                    for i in range(4)]
            t = scounters.thresholds()
            noise = scalibrate.counter_noise(scalibrate._counter_runs(runs), t)
            self.assertEqual(noise["runs"], 4)
            self.assertEqual(noise["stable"], 1)                 # wal_bytes
            self.assertEqual([s["counter"] for s in noise["unstable"]], ["io_extends"])
            self.assertAlmostEqual(noise["instruction_spread_max"], 3e-4, places=6)
            # base 40 (stable), candidate 60 (stable): over 5% and the 8-event floor
            self.assertEqual(noise["trials"], 24)
            self.assertEqual(noise["failing_trials"], 4)
            suggested = scalibrate.suggest(noise, t)
            self.assertEqual(suggested["floor_events"], 40.0)
            self.assertAlmostEqual(suggested["instructions_tolerance"], 0.001)
            self.assertIn("Suggested gate settings", scalibrate.to_markdown(noise, None, suggested))

    def test_jitter_is_not_instability(self):
        # Hosted runners: S10's extends 33 vs 32, a page more or less; under
        # the 8-event floor, so jitter, which the gate doesn't fail on.
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            runs = [counter_run(root, f"r{i}", {"io_extends": 32.0 + i % 2,
                                                "wal_records": 10730.0 + (i % 2) * 900})
                    for i in range(2)]
            t = scounters.thresholds()
            one, two = scalibrate._counter_runs(runs)
            got = {m: v for (_, m), v in scounters.instability(one, two, t).items()}
            self.assertEqual(got, {"io_extends": "jitter", "wal_records": "unstable"})
            runs += [counter_run(root, f"r{i}", {"io_extends": 32.0 + i % 2,
                                                 "wal_records": 10730.0}) for i in (2, 3)]
            noise = scalibrate.counter_noise(scalibrate._counter_runs(runs), t)
            failing = {f["counter"] for f in noise["failing_counters"]}
            self.assertNotIn("io_extends", failing)

    def test_timing_noise(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            run = root / "aa1"
            run.mkdir()
            noise = [1000.0, 1010.0, 990.0, 1005.0, 995.0]
            build_set(run, "base", "heap", noise)
            build_set(run, "candidate", "heap", noise[::-1])
            timing = scalibrate.timing_noise([run])
            self.assertEqual(timing["runs"], 1)
            self.assertEqual(timing["runs_with_a_regressed_cell"], {"5%": 0, "10%": 0})
            cell = next(c for c in timing["cells"] if c["metric"] == "throughput")
            self.assertLess(cell["median_width"], 0.05)


if __name__ == "__main__":
    unittest.main()
