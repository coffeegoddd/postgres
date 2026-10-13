"""Unit tests for the harness parts that need no database.

Run: cd src/tools/prolly_bench && python3 -m unittest discover -s tests
"""

from __future__ import annotations

import json
import random
import tempfile
import unittest
from pathlib import Path

from pbench import report
from pbench.core import config, registry
from pbench.core.interfaces import (CellKey, CellResult, Collector, NotImplementedByEngine,
                                    SqlResult, Status, Workload)
from pbench.core.sqlerr import raise_for_sql_error, sqlstate_of
from pbench.core.stats import LatencyHistogram, per, summarize
from pbench.workloads._common import (check_client_error, parse_pgbench_logs,
                                      parse_pgbench_output)

HERE = Path(__file__).resolve().parents[1]


class HistogramTest(unittest.TestCase):
    def test_percentiles_within_one_percent(self):
        rng = random.Random(1)
        values = [rng.lognormvariate(6, 1) for _ in range(100000)]
        h = LatencyHistogram()
        for v in values:
            h.add(v)
        values.sort()
        for p in (50, 99, 99.9):
            exact = values[int(len(values) * p / 100) - 1]
            self.assertAlmostEqual(h.percentile(p) / exact, 1.0, delta=0.02, msg=p)
        self.assertEqual(h.count, len(values))

    def test_empty(self):
        self.assertIsNone(LatencyHistogram().percentile(50))

    def test_summarize_and_per(self):
        self.assertEqual(summarize([3, None, 1, 2])["median"], 2)
        self.assertEqual(summarize([None])["n"], 0)
        self.assertIsNone(per(5, 0))
        self.assertEqual(per(6, 3), 2)


class PgbenchParsingTest(unittest.TestCase):
    OUTPUT = """pgbench (19devel)
transaction type: <builtin: TPC-B (sort of)>
number of transactions actually processed: 12345
number of failed transactions: 2 (0.016%)
latency average = 0.809 ms
tps = 1234.567890 (without initial connection time)
"""

    def test_output(self):
        d = parse_pgbench_output(self.OUTPUT)
        self.assertEqual(d["transactions"], 12345)
        self.assertEqual(d["failed"], 2)
        self.assertAlmostEqual(d["tps"], 1234.56789)

    def test_logs(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / "pgbench-measure.123").write_text(
                "0 1 500 0 1700000000 1\n0 2 1500 0 1700000000 2\n1 1 failed 0 1 1\n")
            (p / "pgbench-measure.123.1").write_text("2 1 1000 0 1700000000 3\n")
            (p / "pgbench-measure.out").write_text("pgbench -c 4 ...\n")
            h = LatencyHistogram()
            failed = parse_pgbench_logs(p, "pgbench-measure", h)
            self.assertEqual((h.count, failed), (3, 1))
            self.assertFalse((p / "pgbench-measure.123").exists())
            self.assertTrue((p / "pgbench-measure.out").exists())

    def test_not_implemented_from_client_output(self):
        with self.assertRaises(NotImplementedByEngine):
            check_client_error('pgbench: error: query failed: ERROR:  engine '
                               'implementation "d1" does not implement tree_create')
        check_client_error("ERROR:  duplicate key value")  # no exception


class SysbenchParsingTest(unittest.TestCase):
    OUTPUT = """Latency histogram (values are in milliseconds)
       value  ------------- distribution ------------- count
       0.811 |                                         1
       1.000 |****************************************  98
       9.910 |*                                        1

SQL statistics:
    transactions:                        31083  (3107.52 per sec.)
    queries:                             623836 (62367.91 per sec.)
    ignored errors:                      121    (12.10 per sec.)

General statistics:
    total number of events:              31083

Latency (ms):
         99th percentile:                        2.28
"""

    def test_output(self):
        from pbench.workloads.sysbench import parse_sysbench_output
        h = LatencyHistogram()
        d = parse_sysbench_output(self.OUTPUT, h)
        self.assertEqual((d["transactions"], d["tps"]), (31083, 3107.52))
        self.assertEqual((d["queries"], d["ignored_errors"]), (623836, 121))
        self.assertEqual(d["sysbench_p99_ms"], 2.28)
        self.assertEqual(h.count, 100)
        self.assertAlmostEqual(h.percentile(50), 1000, delta=15)
        self.assertAlmostEqual(h.percentile(100), 9910, delta=100)


class SqlErrorTest(unittest.TestCase):
    def test_classify(self):
        err = 'psql:<stdin>:1: ERROR:  0A000: refs implementation "stub" does not implement commit\n'
        self.assertEqual(sqlstate_of(err), "0A000")
        with self.assertRaises(NotImplementedByEngine):
            raise_for_sql_error(SqlResult(False, "", err, "0A000"), "SELECT 1")
        with self.assertRaises(RuntimeError):
            raise_for_sql_error(SqlResult(False, "", "ERROR:  42P01: no table", "42P01"), "x")


class ConfigTest(unittest.TestCase):
    def test_shipped_configs_load(self):
        for name in ("standard.toml", "smoke.toml"):
            cfg = config.load(HERE / "configs" / name, {})
            self.assertEqual(cfg["postgres"]["fsync"], "on")

    def test_fsync_off_refused(self):
        with self.assertRaises(config.ConfigError):
            config.load(HERE / "configs" / "smoke.toml", {"postgres": {"fsync": "off"}})

    def test_unknown_run_key_refused(self):
        with tempfile.NamedTemporaryFile("w", suffix=".toml") as f:
            f.write("[run]\nrepetitons = 3\n")
            f.flush()
            with self.assertRaises(config.ConfigError):
                config.load(Path(f.name), {})


class RegistryTest(unittest.TestCase):
    def test_everything_registered(self):
        registry.load_all()
        names = registry.workloads.names()
        self.assertEqual(sorted((n for n in names if n.startswith("S")),
                                key=lambda n: int(n[1:])),
                         [f"S{i}" for i in range(1, 11)])
        self.assertEqual([n for n in names if n.startswith("V")],
                         [f"V{i}" for i in range(1, 5)])
        for _, cls in registry.workloads.items():
            self.assertTrue(issubclass(cls, Workload))
        for name, cls in registry.collectors.items():
            self.assertTrue(issubclass(cls, Collector))
            self.assertEqual(cls.name, name)
        for name in config.RUN_DEFAULTS["collectors"]:
            registry.collectors.get(name)

    def test_v_variants_include_background(self):
        registry.load_all()
        v2 = registry.workloads.get("V2")()
        labels = [l for l, _ in v2.variants({"changes": [1]})]
        self.assertEqual(labels, ["n1-bgnone", "n1-bgS1"])
        self.assertEqual(v2.client_counts([1, 4], {"background": "none"}), [1])
        self.assertEqual(v2.client_counts([1, 4], {"background": "S1"}), [1, 4])


class ReportTest(unittest.TestCase):
    def test_summary_and_compare(self):
        def cell(w, rep, tps, status=Status.OK):
            return CellResult(key=CellKey("S", w, "tiny", "", 4, rep), status=status,
                              metrics={"tps": tps, "latency_p99_ms": 2.0})
        import dataclasses
        with tempfile.TemporaryDirectory() as d:
            base, other = Path(d) / "base", Path(d) / "other"
            for path, tps in ((base, 100.0), (other, 50.0)):
                path.mkdir()
                with open(path / "results.jsonl", "w") as f:
                    for rep in (1, 2, 3):
                        r = dataclasses.asdict(cell("S1", rep, tps + rep))
                        r["status"] = "ok"
                        f.write(json.dumps(r) + "\n")
            table = report.summary_tables(report.load_results(base), "S")
            self.assertIn("| tiny | - | 4 | ok 3/3 | 102<br>101–103 |", table)
            cmp = report.compare(base, other)
            self.assertIn("0.51", cmp)


if __name__ == "__main__":
    unittest.main()


class TableTest(unittest.TestCase):
    def _set(self, root, label, am, tps, status="ok"):
        d = root / label
        d.mkdir()
        (d / "run.json").write_text(json.dumps(
            {"suite": "S", "label": label, "table_am": am, "started": "2026-10-13T00:00:00"}))
        with open(d / "results.jsonl", "w") as f:
            for rep in (1, 2):
                f.write(json.dumps({
                    "key": {"suite": "S", "workload": "S1", "size": "tiny", "variant": "",
                            "clients": 4, "rep": rep},
                    "status": status, "message": "engine does not implement x",
                    "metrics": {"tps": tps, "latency_p99_ms": 2.0}}) + "\n")
        return d

    def test_engines_side_by_side(self):
        from pbench import table
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            self._set(root, "B0", "heap", 1000.0)
            self._set(root, "prolly_d1", "prolly_d1", 800.0)
            self._set(root, "prolly_d2", "prolly_d2", None, "not_implemented")
            sets = table.discover(root, "S")
            md = table.to_markdown(sets, "S")
            self.assertIn("| B0 (heap) | prolly_d1 | prolly_d2 |", md)
            self.assertIn("| 1,000 | 800 (0.80x) | not impl. |", md)
            self.assertIn("| 2.00 | 2.00 (1.00x) | not impl. |", md)
            page = table.to_html(sets, "S")
            self.assertIn("class='num bad'", page)   # 0.80x throughput is worse
            self.assertIn("not impl.", page)

    def test_write_metrics_na_when_read_only(self):
        from pbench import table
        rs = [{"status": "ok", "read_only": True,
               "metrics": {"wal_bytes_per_txn": 0.01, "tps": 5.0}}]
        self.assertEqual(table._cell(rs, "wal_bytes_per_txn", None).text, "n/a")
        self.assertEqual(table._cell(rs, "throughput", None).text, "5.00")
        rs[0]["read_only"] = False
        self.assertEqual(table._cell(rs, "wal_bytes_per_txn", None).text, "0.01")
