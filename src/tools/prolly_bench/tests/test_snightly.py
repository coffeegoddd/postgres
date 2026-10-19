"""Tests of the S suite nightly's pieces: its shape, its summary for the
perf-history branch (shistory), and its report.

Run: cd src/tools/prolly_bench && python3 -m unittest discover -s tests
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from pbench import scounters, sgate, shistory, sreport
from pbench.core import config
from test_s_suite import HERE, build_set

NOISE = [1000.0, 1010.0, 990.0, 1005.0, 995.0]


def night(root: Path, factor: float) -> Path:
    d = root / f"night-{factor}"
    d.mkdir()
    for am in ("heap", "prolly"):
        build_set(d, "base", am, NOISE)
        build_set(d, "candidate", am, [v * factor for v in NOISE])
    return d


class NightlyTest(unittest.TestCase):
    def test_shape(self):
        cfg = config.load(HERE / "configs" / "s_nightly.toml", {})
        run = cfg["run"]
        self.assertEqual(run["clients"], [1, 8])
        self.assertEqual(run["repetitions"], 5)
        self.assertEqual((run["warmup_seconds"], run["measure_seconds"]), (10, 30))
        # (S5 and S9 are left to it by the pull request's timing)
        self.assertEqual(set(run["workloads"]), {"S1", "S2", "S3", "S4", "S5", "S8", "S9", "S10"})
        self.assertIn("instructions", run["collectors"])
        self.assertEqual(len(cfg["workloads"]["S10"]["tests"]), 3)

    def test_summary(self):
        with tempfile.TemporaryDirectory() as d:
            run = night(Path(d), 0.8)
            t = scounters.thresholds()
            cells = sgate.gate_timing(run, [run, run], t, [])
            timing = {"cells": [c.__dict__ for c in cells], "report_only": True}
            s = shistory.summarize(run, timing, "2026-10-18")
            self.assertEqual(s["schema"], shistory.SCHEMA)
            self.assertEqual(len(s["sets"]), 4)
            cell = s["sets"][0]["cells"][0]
            self.assertEqual((cell["workload"], cell["clients"], cell["n"]), ("S1", 8, 5))
            self.assertTrue(s["confirmed"])
            json.dumps(s)                      # it is what goes to perf-history

    def test_summary_keeps_a_set_without_environment(self):
        with tempfile.TemporaryDirectory() as d:
            run = night(Path(d), 1.0)
            (run / "candidate-B0" / "environment.json").unlink()
            s = shistory.summarize(run, {"cells": []}, "2026-10-18")
            self.assertEqual(len(s["sets"]), 4)
            self.assertIsNone(s["commits"]["candidate"])

    def test_report_opens_on_confirmed_and_clears(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            t = scounters.thresholds()
            for factor, confirmed in ((0.8, True), (1.0, False)):
                run = night(root, factor)
                cells = sgate.gate_timing(run, [run, run], t, [])
                timing = {"cells": [c.__dict__ for c in cells], "report_only": True,
                          "thresholds": t}
                text, passed = sreport.render(None, timing, head="c" * 40, base="b" * 40,
                                              runner="ubuntu-24.04", nightly=True)
                self.assertEqual(passed, not confirmed)
                self.assertIn("## S suite nightly: " + (
                    "4 confirmed regression(s)" if confirmed else "no confirmed regression"), text)
            _, passed = sreport.render(None, None, head="c", base="b", runner="x", nightly=True)
            self.assertFalse(passed)


if __name__ == "__main__":
    unittest.main()
