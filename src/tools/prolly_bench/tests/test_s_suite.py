"""Unit tests for the S suite's harness pieces that need no database.

Run: cd src/tools/prolly_bench && python3 -m unittest discover -s tests
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from pbench.collectors.cachegrind import instructions as cachegrind_instructions
from pbench.collectors.engine_counters import engine_metrics
from pbench.collectors.instructions import parse_perf_stat
from pbench.core import config
from pbench.scounters import CellCounters, CellId, is_counter, kind, unstable

HERE = Path(__file__).resolve().parents[1]


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


class CounterShapeTest(unittest.TestCase):
    def test_counter_configs_load(self):
        for name in ("s_counters.toml", "s_cachegrind.toml"):
            cfg = config.load(HERE / "configs" / name, {})
            self.assertTrue(cfg["run"]["settled_counters"], name)
            self.assertEqual(cfg["run"]["clients"], [1], name)
            self.assertEqual(cfg["postgres"]["autovacuum"], "off", name)
            for wid, params in cfg["workloads"].items():
                if wid not in ("S4", "S13"):
                    self.assertGreater(params["transactions"], 0, wid)

    def test_s5_left_to_timing(self):
        cfg = config.load(HERE / "configs" / "s_counters.toml", {})
        self.assertNotIn("S5", cfg["run"]["workloads"])

    def test_transactions_only_for_fixed_work_clients(self):
        with tempfile.NamedTemporaryFile("w", suffix=".toml") as f:
            f.write("[workloads.S9]\ntransactions = 10\n")
            f.flush()
            with self.assertRaises(config.ConfigError):
                config.load(Path(f.name), {})

    def test_scripts_draw_from_the_seed(self):
        # The server's random() isn't seeded by the run: a script that calls
        # it does different work every run (S7's update did).
        for script in sorted((HERE / "scripts").glob("*.sql")):
            sql = [line for line in script.read_text().splitlines()
                   if not line.startswith(("\\", "--"))]
            self.assertNotRegex("\n".join(sql), r"\brandom\s*\(", script.name)

    def test_perf_stat_parsing(self):
        text = ("# started on ...\n\n"
                "123456,,instructions:u,1000,100.00,,\n"
                "<not supported>,,instructions:k,0,100.00,,\n")
        self.assertEqual(parse_perf_stat(text),
                         {"instructions:u": 123456.0, "instructions:k": None})

    def test_cachegrind_summary(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "cachegrind.out.1"
            p.write_text("desc: I1 cache:\ncmd: postgres\nevents: Ir\nfl=x\n1 5\nsummary: 98765\n")
            self.assertEqual(cachegrind_instructions(p), 98765)

    def _cells(self, **counts):
        return {CellId("S1", "", "counters", 1, "prolly"):
                CellCounters(dict(counts), 2000, "transaction")}

    def test_identical_runs_are_stable(self):
        a = self._cells(wal_bytes=100.0, **{"engine.nodes_read": 5.0})
        self.assertEqual(unstable(a, self._cells(wal_bytes=100.0, **{"engine.nodes_read": 5.0})), [])

    def test_differing_counter_is_unstable(self):
        diffs = unstable(self._cells(wal_bytes=100.0), self._cells(wal_bytes=101.0))
        self.assertEqual([(m, x, y) for _, m, x, y in diffs], [("wal_bytes", 100.0, 101.0)])

    def test_instructions_within_tolerance(self):
        a, b = self._cells(instructions_user=1000000.0), self._cells(instructions_user=1000500.0)
        self.assertEqual(unstable(a, b, 0.001), [])
        self.assertEqual(len(unstable(a, b, 0.0001)), 1)

    def test_only_totals_are_counters(self):
        self.assertTrue(is_counter("engine.nodes_read"))
        self.assertFalse(is_counter("engine.nodes_read_per_operation"))
        self.assertFalse(is_counter("tps"))
        self.assertEqual(kind("wal_bytes"), "bytes")
        self.assertEqual(kind("wal_records"), "events")


if __name__ == "__main__":
    unittest.main()
