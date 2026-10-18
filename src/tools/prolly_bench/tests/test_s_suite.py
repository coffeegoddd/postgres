"""Unit tests for the S suite's pull-request gate: its counters, run
shapes, pairing by build, gate and report.  No database needed.

Run: cd src/tools/prolly_bench && python3 -m unittest discover -s tests
"""

from __future__ import annotations

import unittest

from pbench.collectors.engine_counters import engine_metrics


class EngineCountersTest(unittest.TestCase):
    def test_totals_and_per_operation(self):
        before = {"nodes_read": 10.0, "commits": 1.0}
        after = {"nodes_read": 30.0, "commits": 3.0}
        m = engine_metrics(before, after, 4)
        self.assertEqual(m["engine.nodes_read"], 20.0)
        self.assertEqual(m["engine.nodes_read_per_operation"], 5.0)
        self.assertEqual(m["engine.commits"], 2.0)
        self.assertEqual(m["engine.commits_per_operation"], 0.5)

    def test_reset_during_window_refused(self):
        with self.assertRaises(ValueError):
            engine_metrics({"commits": 5.0}, {"commits": 1.0}, 1)


if __name__ == "__main__":
    unittest.main()
