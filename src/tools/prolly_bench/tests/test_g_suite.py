"""The G and GV suites' workloads: registrations, configurations, the
generator's determinism, and the answer gate."""
import json
import tempfile
import unittest
from pathlib import Path

from evidence_helpers import seal
from pbench.core import config, registry, suites
from pbench.core.validation import compatible
from pbench.workloads import g_suite

HERE = Path(__file__).resolve().parents[1]


class GSuiteTest(unittest.TestCase):
    def test_every_workload_registers_with_its_suite(self):
        registry.load_all()
        names = {n for n in registry.workloads.names() if suites.of_workload(n).letter in ("G", "GV")}
        self.assertEqual(names, {f"G{i}" for i in range(1, 10)} | {f"GV{i}" for i in range(1, 6)})
        for name in names:
            cls = registry.workloads.get(name)
            self.assertEqual(cls.suite, suites.of_workload(name).letter, name)
            self.assertEqual(cls.id, name)
        self.assertEqual(set(g_suite.PARAMS), names)

    def test_configs_load(self):
        for letter in ("G", "GV"):
            for name in ("standard.toml", "g_smoke.toml"):
                cfg = config.load(HERE / "configs" / name, {}, suite=letter)
                for size in cfg["run"]["sizes"]:
                    self.assertTrue(g_suite.SIZE_KEYS <= set(cfg["sizes"][size]), (letter, name, size))

    def test_unknown_choices_are_refused(self):
        prefix = '[run]\nsizes = ["t"]\n[sizes.t]\ng_points = 10\n'
        with tempfile.TemporaryDirectory() as d:
            for text in ('[workloads.GV2]\noverlap = ["sideways"]\n',
                         '[workloads.G2]\npredicates = ["contains"]\n',
                         '[workloads.GV1]\nbackground = ["S9"]\n',
                         # (a background of the other suite's)
                         '[workloads.GV1]\nbackground = ["S1"]\n',
                         '[workloads.V2]\nbackground = ["G5"]\n',
                         '[workloads.G4]\nnot_a_parameter = 1\n'):
                path = Path(d) / "c.toml"
                path.write_text(prefix + text)
                with self.assertRaises(config.ConfigError, msg=text):
                    config.load(path, {}, suite="GV" if "GV" in text else "V" if "V2" in text else "G")
            path.write_text(prefix + '[workloads.GV1]\nbackground = ["none", "G5"]\n')
            config.load(path, {}, suite="GV")

    def test_generator_is_fixed(self):
        # the probes and the hash the generator's SQL uses are the same in
        # every run (no random(), no seed from the clock)
        self.assertEqual(g_suite.probes(3, 21), g_suite.probes(3, 21))
        self.assertNotEqual(g_suite.probes(3, 21), g_suite.probes(3, 22))
        self.assertTrue(all(0 <= x < g_suite.EXTENT and 0 <= y < g_suite.EXTENT
                            for x, y in g_suite.probes(100, 7)))
        for t, (columns, rows) in g_suite.TABLES.items():
            sql = rows(type("C", (), {"size_params": {}})())
            self.assertNotIn("random(", sql, t)

    def test_g5_counts_each_script(self):
        out = ("SQL script 1: g5_move.sql\n - weight: 8 (targets 80.0% of total)\n"
               " - 6956 transactions (78.9% of total)\n"
               "SQL script 2: g5_insert.sql\n - 900 transactions (10.2% of total)\n"
               "SQL script 3: g5_delete.sql\n - 896 transactions (10.2% of total)\n")
        self.assertEqual(g_suite.G5.script_transactions(out), [6956, 900, 896])
        with self.assertRaises(RuntimeError):
            g_suite.G5.script_transactions(" - 10 transactions (100.0% of total)\n")

    def test_variants(self):
        self.assertEqual([v for v, _ in g_suite.G3().variants({"k": [1, 100]})], ["k1", "k100"])
        self.assertEqual([v for v, _ in g_suite.GV2().variants({"changes": [10], "background": ["none"]})],
                         ["n10-disjoint-bgnone", "n10-overlapping-bgnone"])
        self.assertEqual(g_suite.GV1().client_counts([1, 4], {"background": "none"}), [1])
        self.assertEqual(g_suite.GV1().client_counts([1, 4], {"background": "G5"}), [1, 4])

    def test_answers_must_agree(self):
        with tempfile.TemporaryDirectory() as tmp:
            base, engine = Path(tmp) / "base", Path(tmp) / "engine"
            for p in (base, engine):
                (p / "raw").mkdir(parents=True)
                (p / "raw/answers.json").write_text(json.dumps({"window0": "a", "window1": "b"}))
                (p / "results.jsonl").write_text(json.dumps({"key": {"suite": "G", "workload": "G2"},
                    "status": "ok", "raw_dir": "raw", "metrics": {}}) + "\n")
                seal(p)
            self.assertEqual(compatible(base, engine), [])
            (engine / "raw/answers.json").write_text(json.dumps({"window0": "a", "window1": "c"}))
            seal(engine)
            problems = compatible(base, engine)
            self.assertTrue(any("answers differ (window1)" in p for p in problems), problems)
            # one that recorded none, where the other did, differs too
            (engine / "raw/answers.json").unlink()
            seal(engine)
            self.assertTrue(compatible(base, engine))


if __name__ == "__main__":
    unittest.main()
