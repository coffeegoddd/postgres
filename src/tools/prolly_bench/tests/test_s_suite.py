"""Unit tests for the S suite's harness pieces that need no database.

Run: cd src/tools/prolly_bench && python3 -m unittest discover -s tests
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from evidence_helpers import seal
from pbench import bcompare, table
from pbench.core.interfaces import CellKey
from pbench.core.runner import interleave
from pbench.core.validation import compatible
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

    def test_perf_stat_hybrid_pmus_summed(self):
        text = ("100,,cpu_core/instructions:u/,1000,100.00,,\n"
                "<not counted>,,cpu_atom/instructions:u/,0,0.00,,\n"
                "<not supported>,,cpu_core/instructions:k/,0,100.00,,\n")
        self.assertEqual(parse_perf_stat(text),
                         {"instructions:u": 100.0, "instructions:k": None})

    def test_skipped_transactions_count_toward_fixed_work(self):
        from pbench.workloads._common import PgbenchResult
        res = PgbenchResult(transactions=1990, failed=0, skipped=10)
        self.assertEqual(res.attempted(), 2000)

    def test_settled_counters_refuse_s6(self):
        for workloads in ([], ["S1", "S6"]):
            with self.assertRaises(config.ConfigError, msg=workloads):
                config.load(HERE / "configs" / "s_counters.toml",
                            {"run": {"workloads": workloads}})

    def test_every_io_counter_is_compared(self):
        for m in ("io_evictions", "io_fsyncs", "io_read_bytes", "io_write_bytes"):
            self.assertTrue(is_counter(m), m)
        self.assertEqual(kind("io_read_bytes"), "bytes")

    def test_cachegrind_counts_a_reused_pid(self):
        from pbench.collectors.cachegrind import written
        with tempfile.TemporaryDirectory() as d:
            counts = Path(d)
            f = counts / "cachegrind.out.123"
            f.write_text("summary: 1\n")
            before = written(counts)
            f.write_text("summary: 22\n")   # pid 123 again, in the window
            self.assertNotEqual(written(counts)[f], before[f])

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


def build_set(root: Path, build: str, am: str, tps: list[float], *, identity="test",
              workload="S1", seeds=None) -> Path:
    """A sealed S result set of one build and engine: a cell per repetition."""
    label = f"{build}-{'B0' if am == 'heap' else am}"
    d = root / label
    d.mkdir()
    (d / "run.json").write_text(json.dumps(
        {"suite": "S", "label": label, "table_am": am, "build": build,
         "started": "2026-10-18T00:00:00"}))
    with open(d / "results.jsonl", "w") as f:
        for rep, value in enumerate(tps, 1):
            f.write(json.dumps({
                "key": {"suite": "S", "workload": workload, "size": "pr", "variant": "",
                        "clients": 8, "rep": rep, "build": build},
                "status": "ok", "metrics": {"tps": value, "latency_p50_ms": 8000.0 / value},
                "measurement": {"seed": (seeds or {}).get(rep, 1 + rep), "seconds": 20,
                                "quality": "smoke"}}) + "\n")
    seal(d)
    run = json.loads((d / "run.json").read_text())
    run["comparison_identity"] = identity
    (d / "run.json").write_text(json.dumps(run))
    return d


class BuildPairingTest(unittest.TestCase):
    def test_pg_bin_builds(self):
        from pbench.__main__ import pg_bins
        self.assertEqual(pg_bins("/x/bin"), [("", Path("/x/bin"))])
        self.assertEqual(pg_bins("base=/a,candidate=/b"),
                         [("base", Path("/a")), ("candidate", Path("/b"))])
        for bad in ("base=/a", "base=/a,base=/b", "=/a,candidate=/b", "base=/a,candidate"):
            with self.assertRaises(SystemExit, msg=bad):
                pg_bins(bad)

    def test_key_paths(self):
        k = CellKey("S", "S1", "pr", "", 8, 3, build="candidate")
        self.assertEqual(k.pair_path(), Path("S1/pr/c8/r3"))
        self.assertEqual(k.path(), Path("candidate/S1/pr/c8/r3"))
        self.assertEqual(CellKey("S", "S1", "pr", "", 8, 3).path(), Path("S1/pr/c8/r3"))

    def test_builds_and_engines_run_in_groups(self):
        plans = []
        for build in ("base", "candidate"):
            for am in ("heap", "prolly"):
                plans.append([(CellKey("S", w, "pr", "", 8, rep, build=build), {"am": am})
                              for w in ("S1", "S2") for rep in (1, 2, 3)])
        order = interleave(plans, 7)
        self.assertEqual(len(order), 24)
        groups = [order[i:i + 4] for i in range(0, 24, 4)]
        for g in groups:
            self.assertEqual(len({str(k.pair_path()) for _, k, _ in g}), 1)
            self.assertEqual({i for i, _, _ in g}, {0, 1, 2, 3})
        # (in random order: not always the base first)
        self.assertGreater(len({tuple(i for i, _, _ in g) for g in groups}), 1)

    def test_a_a_ratios_are_one(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            noise = [1000.0, 1010.0, 990.0, 1005.0, 995.0]
            build_set(root, "base", "heap", noise)
            build_set(root, "candidate", "heap", [noise[i] for i in (1, 0, 3, 2, 4)])
            build_set(root, "base", "prolly", [v * 0.8 for v in noise])
            build_set(root, "candidate", "prolly", [v * 0.8 for v in noise])
            rows = bcompare.compare([table.load_set(p) for p in sorted(root.iterdir())])
            self.assertEqual([r.engine for r in rows], ["heap", "prolly"])
            for row in rows:
                r = row.ratios["throughput"]
                self.assertEqual(row.problems, [])
                self.assertLessEqual(r.low, 1.0)
                self.assertGreaterEqual(r.high, 1.0)
                self.assertAlmostEqual(r.estimate, 1.0, delta=0.02)
                self.assertNotEqual(bcompare.regression(r, True, 0.05), "regressed")
            self.assertAlmostEqual(rows[1].over_heap["base"], 0.8)
            self.assertAlmostEqual(rows[1].over_heap["candidate"], 0.8)
            md = bcompare.to_markdown(rows)
            self.assertIn("| S1 | - | 8 | prolly | 1.00 [", md)
            self.assertIn("0.80 → 0.80", md)

    def test_slowed_candidate_detected(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            noise = [1000.0, 1010.0, 990.0, 1005.0, 995.0]
            build_set(root, "base", "heap", noise)
            build_set(root, "candidate", "heap", [v * 0.85 for v in noise])
            rows = bcompare.compare([table.load_set(p) for p in sorted(root.iterdir())])
            self.assertEqual(bcompare.regression(rows[0].ratios["throughput"], True, 0.05),
                             "regressed")
            self.assertEqual(bcompare.regression(rows[0].ratios["latency_p50_ms"], False, 0.05),
                             "regressed")

    def test_unpaired_seeds_are_not_compared(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            build_set(root, "base", "heap", [1000.0] * 5)
            build_set(root, "candidate", "heap", [1000.0] * 5, seeds={3: 99})
            rows = bcompare.compare([table.load_set(p) for p in sorted(root.iterdir())])
            self.assertIn("repetition 3 ran with different seeds", rows[0].problems)
            self.assertIsNone(rows[0].ratios["throughput"].low)   # 4 pairs: no interval

    def test_compatible_varies_only_the_build(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            base = build_set(root, "base", "heap", [1000.0] * 5)
            cand = build_set(root, "candidate", "heap", [1000.0] * 5)
            cand_prolly = build_set(root, "candidate", "prolly", [1000.0] * 5)
            (root / "elsewhere").mkdir()
            other_machine = build_set(root / "elsewhere", "candidate", "heap", [1000.0] * 5,
                                      identity="elsewhere")
            self.assertEqual(compatible(base, cand, "build"), [])
            self.assertTrue(compatible(base, cand_prolly, "build"))      # engine differs too
            self.assertTrue(compatible(base, base, "build"))             # one build
            self.assertTrue(compatible(base, other_machine, "build"))    # conditions differ
            self.assertTrue(compatible(base, cand, "engine"))            # builds differ

    def test_comparison_only_may_relax_commit(self):
        cfg = config.load(HERE / "configs" / "s_pr.toml", {})
        self.assertEqual(cfg["postgres"]["synchronous_commit"], "off")
        self.assertEqual(cfg["postgres"]["fsync"], "on")
        self.assertEqual(cfg["run"]["repetitions"], 5)
        with self.assertRaises(config.ConfigError):
            config.load(HERE / "configs" / "s_pr.toml", {"run": {"comparison_only": False}})
        with self.assertRaises(config.ConfigError):
            config.load(HERE / "configs" / "s_pr.toml", {"postgres": {"fsync": "off"}})


if __name__ == "__main__":
    unittest.main()
