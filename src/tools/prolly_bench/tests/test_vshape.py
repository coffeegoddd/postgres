"""Tests of the V suite's growth-shape gate (vshape) and its workload, V9:
shapes from counter series, targets met and missed, known exceptions that
regress, improve or meet their target, and a run's cells.

Run: cd src/tools/prolly_bench && python3 -m unittest discover -s tests
"""

from __future__ import annotations

import argparse
import json
import math
import tempfile
import unittest
from pathlib import Path

from pbench import vshape
from pbench.workloads.v_suite import V9

LEVELS = [4000, 16000, 64000, 256000]


def known(check="point_read", sweep="rows", counter="nodes_read", current="linear"):
    return vshape.Known(check, sweep, counter, current, "#697", "a full scan")


class ShapeTest(unittest.TestCase):
    def shape(self, values, floor=1.0):
        return vshape.shape(LEVELS, values, floor)[0]

    def test_shapes(self):
        self.assertEqual(self.shape([0, 0, 0, 0]), "zero")
        self.assertEqual(self.shape([27, 27, 27, 27]), "flat")
        # a tree's descent: a level deeper every so often
        self.assertEqual(self.shape([85, 95, 114, 126]), "log")
        self.assertEqual(self.shape([n / 10 for n in LEVELS]), "linear")
        self.assertEqual(self.shape([(n / 1000) ** 2 for n in LEVELS]), "super")

    def test_floor_hides_small_counts(self):
        self.assertEqual(self.shape([3, 10, 10, 10]), "linear")
        self.assertEqual(self.shape([3, 10, 10, 10], floor=64), "flat")

    def test_fastest_growth_between_levels(self):
        # rows written back until the reset shares storage instead: linear
        # where it writes them, whatever the sweep's end
        self.assertEqual(self.shape([700, 2800, 0, 0], floor=8), "linear")
        b = vshape.growth(LEVELS, [700, 2800, 0, 0], 8)
        self.assertAlmostEqual(b, 1.0)

    def test_sweep_must_span(self):
        with self.assertRaises(ValueError):
            vshape.growth([1, 2, 3, 4], [1, 1, 1, 1], 1)
        with self.assertRaises(ValueError):
            vshape.growth([1, 10, 100], [1, 1, 1], 1)
        with self.assertRaises(ValueError):
            vshape.growth([100, 10, 1000, 10000], [1, 1, 1, 1], 1)


class JudgeTest(unittest.TestCase):
    linear = [n / 10 for n in LEVELS]

    def judge(self, target, values, k=None):
        return vshape.judge("point_read", "rows", "nodes_read", target, LEVELS, values, k)

    def test_target_met_and_missed(self):
        self.assertEqual(self.judge("log", [85, 95, 114, 126]).result, "met")
        self.assertEqual(self.judge("linear", [27] * 4).result, "met")
        f = self.judge("log", self.linear)
        self.assertEqual((f.result, f.measured), ("unmet", "linear"))
        self.assertTrue(f.fails)

    def test_known_exception_stays_checked(self):
        f = self.judge("log", self.linear, known())
        self.assertEqual((f.result, f.issue), ("known", "#697"))
        self.assertFalse(f.fails)
        # grows faster than recorded: a regression
        f = self.judge("log", [(n / 1000) ** 2 for n in LEVELS], known())
        self.assertEqual(f.result, "known_regressed")
        self.assertTrue(f.fails)
        # slower, still short of the target: the entry is to say so
        self.assertEqual(self.judge("flat", [85, 95, 114, 126], known()).result, "known_improved")
        # the target met: the entry is to go, and the check is enforced
        f = self.judge("log", [85, 95, 114, 126], known())
        self.assertEqual(f.result, "known_met")
        self.assertTrue(f.fails)

    def test_zero_target(self):
        self.assertEqual(self.judge("zero", [0, 0, 0, 0]).result, "met")
        self.assertEqual(self.judge("zero", [10, 10, 10, 10]).result, "unmet")

    def test_missing_counter_fails(self):
        f = vshape.judge("point_read", "rows", "nodes_read", "log", LEVELS, None, None)
        self.assertEqual(f.result, "missing")
        self.assertTrue(f.fails)


def series(checks=None):
    """A V9 cell's series: the rows sweep's checks, every counter of every
    target 0 but those given."""
    out = {}
    for sweep in ("rows", "delta", "catalog"):
        cs = {}
        for (check, sw), counters in vshape.TARGETS.items():
            if sw == sweep:
                cs[check] = {c: [0] * 4 for c in vshape.WORK + vshape.CATALOG}
        for (check, sw, counter), values in (checks or {}).items():
            if sw == sweep:
                cs[check][counter] = values
        out[sweep] = {"levels": LEVELS, "checks": cs}
    return out


class GateTest(unittest.TestCase):
    def test_all_zero_meets_every_target(self):
        findings = vshape.gate([series()], [])
        self.assertTrue(findings)
        self.assertEqual({f.result for f in findings}, {"met"})

    def test_unmatched_entry_fails(self):
        findings = vshape.gate([series()], [known(check="renamed")])
        f = [f for f in findings if f.result == "unmatched"]
        self.assertEqual(len(f), 1)
        self.assertTrue(f[0].fails)

    def test_cell_findings(self):
        cell = series({("point_read", "rows", "nodes_read"): [n / 10 for n in LEVELS],
                         ("first_use", "rows", "bmap_refcount_updates"): [n / 10 for n in LEVELS]})
        findings = {(f.check, f.sweep, f.counter): f for f in vshape.gate([cell], [known()])}
        self.assertEqual(findings[("point_read", "rows", "nodes_read")].result, "known")
        self.assertEqual(findings[("first_use", "rows", "bmap_refcount_updates")].result, "unmet")

    def run_dir(self, root: Path, cell: dict) -> Path:
        raw = root / "raw" / "V9"
        raw.mkdir(parents=True)
        (raw / "v9-series.json").write_text(json.dumps(cell))
        (root / "results.jsonl").write_text(json.dumps(
            {"key": {"workload": "V9"}, "status": "ok", "raw_dir": "raw/V9"}) + "\n")
        return root

    def test_run(self):
        with tempfile.TemporaryDirectory() as d:
            root = self.run_dir(Path(d), series({("point_read", "rows", "nodes_read"):
                                                   [n / 10 for n in LEVELS]}))
            path = root / "known.toml"
            path.write_text('[[known]]\ncheck = "point_read"\nsweep = "rows"\ncounter = "nodes_read"\n'
                            'current = "linear"\nissue = "#697"\nreason = "a full scan"\n')
            args = argparse.Namespace(dir=root, known=path, json=root / "out.json",
                                      markdown=root / "out.md")
            self.assertEqual(vshape.run(args), 0)
            out = json.loads((root / "out.json").read_text())
            self.assertTrue(out["passed"])
            self.assertIn("| point_read | rows | nodes_read | log | linear |", (root / "out.md").read_text())
            # without the entry, the exception fails
            args.known = None
            self.assertEqual(vshape.run(args), 1)


class KnownFileTest(unittest.TestCase):
    def load(self, text):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "known.toml"
            path.write_text(text)
            return vshape.load_known(path)

    ENTRY = ('[[known]]\ncheck = "point_read"\nsweep = "rows"\ncounter = "nodes_read"\n'
             'current = "linear"\nissue = "#697"\nreason = "a full scan"\n')

    def test_entries(self):
        self.assertEqual(self.load(self.ENTRY), [known()])
        for bad in (self.ENTRY.replace('issue = "#697"', 'issue = "697"'),
                    self.ENTRY.replace('"linear"', '"quadratic"'),
                    self.ENTRY.replace('reason = "a full scan"\n', ''),
                    self.ENTRY + self.ENTRY,
                    "[[accept]]\n"):
            with self.assertRaises(ValueError, msg=bad):
                self.load(bad)

    def test_checked_in_entries_load_and_name_targets(self):
        path = Path(__file__).resolve().parents[1] / "configs" / "v_shape_known.toml"
        # (none, once every target is met)
        for e in vshape.load_known(path):
            target = vshape.TARGETS[(e.check, e.sweep)][e.counter]
            # (an exception is only for a shape that misses its target)
            self.assertGreater(vshape.rank(e.current), vshape.rank(target), e)


class V9Test(unittest.TestCase):
    def test_config_runs_nothing_in_the_background(self):
        # A fold worker's or autovacuum's work counts in the database's
        # pg_stat_prolly too, and changes what an operation finds (a working
        # set folded already): how much of it lands in a sweep depends on
        # the machine's speed.
        import tomllib
        path = Path(__file__).resolve().parents[1] / "configs" / "v_shape.toml"
        with open(path, "rb") as f:
            settings = tomllib.load(f)["postgres"]
        self.assertEqual(settings.get("prolly_fold_worker"), "off")
        self.assertEqual(settings.get("autovacuum"), "off")

    def test_every_counted_step_has_targets(self):
        labels = [label for label, _ in V9().steps(64000, 10) if label]
        self.assertEqual(len(labels), len(set(labels)))
        checked = {check for check, _ in vshape.TARGETS}
        self.assertEqual(set(labels), checked)

    def test_unrelated_objects(self):
        sql = V9.unrelated(512)
        # K shared tables, and K on another branch made before the table's
        self.assertEqual(sql.count("FOR g IN 1..512 LOOP"), 2)
        self.assertIn("CREATE SCHEMA v9_plain;", sql)
        self.assertLess(sql.index("dolt_commit"), sql.index("dolt_branch('v9_other')"))
        self.assertTrue(sql.rstrip().endswith("SELECT dolt_checkout('main');"))

    def test_catalog_reading_reads_no_catalog(self):
        # (pg_stat_get_blocks_fetched over every OID a system catalog can
        # have: no catalog scanned, so the reading doesn't grow with them)
        self.assertNotIn("pg_class", V9.READ)
        self.assertIn("catalog_blocks_read", V9.READ)

    def test_changes_are_delta_distinct_rows(self):
        sql = V9.change(64000, 700, 1)
        self.assertIn("generate_series(0, 699)", sql)
        self.assertIn("1 + i * 91", sql)
        # (the main branch's change is past every changed row)
        self.assertLess(1 + 699 * 91, 64000)

    def test_sweeps_validate(self):
        class Ctx:
            params = {}
        self.assertEqual([p[0] for p in V9().sweeps(Ctx)["rows"]], LEVELS)
        self.assertEqual([p[2] for p in V9().sweeps(Ctx)["catalog"]], [32, 128, 512, 2048])
        # (the rows and delta sweeps add no unrelated objects)
        self.assertEqual({p[2] for p in V9().sweeps(Ctx)["rows"] + V9().sweeps(Ctx)["delta"]}, {0})
        for bad in ({"rows": [100, 200, 400, 800], "delta": 100},
                    {"rows": [1000, 2000, 4000]},
                    {"deltas": [10, 20, 40, 80, 64000]},
                    {"others": [0, 10, 100, 1000]},
                    {"others": [10, 20, 40, 60]}):
            Ctx.params = bad
            with self.assertRaises(ValueError, msg=bad):
                V9().sweeps(Ctx)


if __name__ == "__main__":
    unittest.main()
