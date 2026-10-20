"""Tests of the V suite's growth gate (vgrowth): the ratio of a series'
last tenth's mean to its first's, a linear-growth series failing the gate
and a flat one passing, floors, accepted growth, and a run's cells.

Run: cd src/tools/prolly_bench && python3 -m unittest discover -s tests
"""

from __future__ import annotations

import argparse
import json
import math
import tempfile
import unittest
from pathlib import Path

from pbench import vgrowth
from pbench.sgate import Acceptance

K = 2000


def cell(variant: str, series: dict[str, list[float]]) -> dict:
    metrics = {}
    for c in vgrowth.FLOORS:
        first, last = vgrowth.means(series.get(c, [0.0] * K))
        metrics[f"first_{c}"], metrics[f"last_{c}"] = first, last
    return {"key": {"workload": "V7", "variant": variant}, "metrics": metrics}


class RatioTest(unittest.TestCase):
    def test_ratio(self):
        self.assertEqual(vgrowth.ratio(4.0, 6.0), 1.5)
        self.assertEqual(vgrowth.ratio(0.0, 0.0), 1.0)
        self.assertTrue(math.isinf(vgrowth.ratio(0.0, 1.0)))

    def test_means_of_tenths(self):
        first, last = vgrowth.means([float(i) for i in range(100)])
        self.assertEqual((first, last), (4.5, 94.5))
        with self.assertRaises(ValueError):
            vgrowth.means([1.0])


class GateTest(unittest.TestCase):
    def judge(self, series, counter="fold_rows_read", accept=()):
        first, last = vgrowth.means(series)
        return vgrowth.judge("V7/owned", "prolly", counter, first, last,
                             vgrowth.DEFAULT_BOUND, list(accept))

    def test_linear_growth_fails(self):
        # each commit folds every entry since the last full fold: the
        # per-transaction series grows linearly (the run, quadratically)
        f = self.judge([float(i + 1) for i in range(K)])
        self.assertEqual(f.result, "grows")
        self.assertTrue(f.fails)
        self.assertGreater(f.ratio, 9)

    def test_flat_passes(self):
        f = self.judge([100.0 + (i % 3) for i in range(K)])
        self.assertEqual(f.result, "flat")
        self.assertFalse(f.fails)

    def test_floor(self):
        # 0 at first and 1 later is more than the bound, but not work
        f = self.judge([0.0] * (K // 2) + [1.0] * (K // 2))
        self.assertEqual(f.result, "flat")
        # a full fold where there was none counts from the first
        f = self.judge([0.0] * (K // 2) + [1.0] * (K // 2), counter="fold_full_tables")
        self.assertEqual(f.result, "grows")

    def test_accepted(self):
        known = Acceptance("V7/owned", "prolly", "fold_rows_read", "#569")
        f = self.judge([float(i + 1) for i in range(K)], accept=[known])
        self.assertEqual((f.result, f.reason), ("accepted", "#569"))
        self.assertFalse(f.fails)
        # accepted, and flat now: fails, so the entry is removed rather
        # than left to accept a later regression
        f = self.judge([5.0] * K, accept=[known])
        self.assertEqual(f.result, "accepted_flat")
        self.assertTrue(f.fails)

    def test_ungated(self):
        f = vgrowth.judge("V7/manual", "prolly", "wal_bytes", 100.0, 1e6,
                          vgrowth.DEFAULT_BOUND, [], gated=False)
        self.assertEqual(f.result, "ungated")
        self.assertFalse(f.fails)

    def test_missing(self):
        f = vgrowth.judge("V7/owned", "prolly", "fold_rows_read", None, None,
                          vgrowth.DEFAULT_BOUND, [])
        self.assertTrue(f.fails)


class MetricsTest(unittest.TestCase):
    def test_infinite_growth_is_written(self):
        # 0 at first, then not: growth with no finite ratio, which a cell's
        # results must still hold (they are JSON, finite numbers only)
        s = [0.0] * (K // 2) + [1.0] * (K // 2)
        m = vgrowth.growth_metrics({"fold_full_tables": s})
        self.assertIsNone(m["growth_fold_full_tables"])
        self.assertEqual((m["first_fold_full_tables"], m["last_fold_full_tables"]), (0.0, 1.0))
        json.dumps(m, allow_nan=False)
        f = vgrowth.judge("V7/shared", "prolly", "fold_full_tables",
                          m["first_fold_full_tables"], m["last_fold_full_tables"],
                          vgrowth.DEFAULT_BOUND, [])
        self.assertEqual(f.result, "grows")

    def test_missing_counter(self):
        m = vgrowth.growth_metrics({"log_pages_read": None})
        self.assertIsNone(m["first_log_pages_read"])
        f = vgrowth.judge("V7/shared", "prolly", "log_pages_read", None, None,
                          vgrowth.DEFAULT_BOUND, [])
        self.assertEqual(f.result, "missing")

    def test_ungated_marked(self):
        m = vgrowth.growth_metrics({"wal_bytes": [1.0] * K}, {"wal_bytes"})
        self.assertEqual(m["ungated_wal_bytes"], 1.0)


class RunTest(unittest.TestCase):
    def run_gate(self, rows, accept=None):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "results.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows))
            (d / "run.json").write_text(json.dumps({"table_am": "prolly"}))
            path = None
            if accept:
                path = d / "accept.toml"
                path.write_text(accept)
            args = argparse.Namespace(dir=d, bound=vgrowth.DEFAULT_BOUND, accept=path,
                                      json=d / "out.json")
            code = vgrowth.run(args)
            out = d / "out.json"
            return code, json.loads(out.read_text()) if out.exists() else None

    def test_run(self):
        flat = cell("shared-bgnone", {"fold_rows_read": [1.0] * K})
        grows = cell("owned-bgnone", {"fold_rows_read": [float(i) for i in range(K)]})
        code, out = self.run_gate([flat])
        self.assertEqual((code, out["passed"]), (0, True))
        code, out = self.run_gate([flat, grows])
        self.assertEqual(code, 1)
        self.assertEqual([(f["workload"], f["counter"]) for f in out["findings"]
                          if f["result"] == "grows"], [("V7/owned-bgnone", "fold_rows_read")])
        code, out = self.run_gate([flat, grows], accept=(
            '[[accept]]\nworkload = "V7/owned-bgnone"\nengine = "prolly"\n'
            'counter = "fold_rows_read"\nreason = "#569"\n'))
        self.assertEqual(code, 0)

    def test_unmatched_entry_fails(self):
        flat = cell("shared-bgnone", {"fold_rows_read": [1.0] * K})
        code, out = self.run_gate([flat], accept=(
            '[[accept]]\nworkload = "V7/renamed-bgnone"\nengine = "prolly"\n'
            'counter = "fold_rows_read"\nreason = "#569"\n'))
        self.assertEqual(code, 1)
        self.assertEqual([f["result"] for f in out["findings"] if f["workload"] == "V7/renamed-bgnone"],
                         ["unmatched"])

    def test_missing_accept_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "results.jsonl").write_text("")
            (d / "run.json").write_text(json.dumps({"table_am": "prolly"}))
            args = argparse.Namespace(dir=d, bound=vgrowth.DEFAULT_BOUND,
                                      accept=d / "no-such.toml", json=None)
            with self.assertRaises(SystemExit):
                vgrowth.run(args)

    def test_ungated_cell(self):
        noisy = cell("manual-bgnone", {"wal_bytes": [float(i) for i in range(K)]})
        noisy["metrics"]["ungated_wal_bytes"] = 1.0
        code, out = self.run_gate([noisy])
        self.assertEqual(code, 0)
        self.assertEqual([f["result"] for f in out["findings"] if f["counter"] == "wal_bytes"],
                         ["ungated"])

    def test_no_cells_fails(self):
        code, _ = self.run_gate([{"key": {"workload": "V1", "variant": ""}, "metrics": {}}])
        self.assertEqual(code, 1)


if __name__ == "__main__":
    unittest.main()
