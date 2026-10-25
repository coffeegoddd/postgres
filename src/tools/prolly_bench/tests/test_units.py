"""Unit tests for the harness parts that need no database.

Run: cd src/tools/prolly_bench && python3 -m unittest discover -s tests
"""

from __future__ import annotations

import gzip
import json
import random
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from pbench import report
from evidence_helpers import seal
from unittest.mock import patch
from pbench.core import config, registry, targets
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

    def test_missing_latency_logs_cannot_claim_complete_measurement(self):
        from pbench.workloads import _common
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            ctx = SimpleNamespace(key=SimpleNamespace(clients=1), params={},
                scratch_dir=p, raw_dir=p, dbname="test", session_options=lambda: {},
                cluster=SimpleNamespace(bin=lambda name: name, client_env=lambda options: {}))
            result = SimpleNamespace(returncode=0, stderr="", stdout=self.OUTPUT.replace("12345", "1")
                                     .replace("transactions: 2", "transactions: 0"))
            (p / "pgbench-measure.123").write_text("0 1 500 0 1700000000 1\n")
            with patch.object(_common.process, "run", return_value=result):
                self.assertEqual(_common.run_pgbench(ctx, seconds=1).latency.count, 1)
                with self.assertRaisesRegex(RuntimeError, "latency records"):
                    _common.run_pgbench(ctx, seconds=1)

    def test_not_implemented_from_client_output(self):
        with self.assertRaises(NotImplementedByEngine):
            check_client_error('pgbench: error: query failed: ERROR:  reclamation '
                               'implementation "stub" does not implement collect')
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


class SysbenchDoltScriptsTest(unittest.TestCase):
    def _ctx(self, test, scripts="/scripts"):
        cluster = SimpleNamespace(client_env=lambda options=None: {"PGHOST": "/tmp/s", "PGPORT": "5432"})
        return SimpleNamespace(params={"test": test}, size_params={"sysbench_tables": 4,
                                                                   "sysbench_table_size": 500},
                               key=SimpleNamespace(clients=2), cluster=cluster, dbname="bench",
                               tools={"sysbench_lua_scripts": scripts})

    def test_dolt_is_every_test_dolt_runs(self):
        from pbench.workloads.sysbench import DOLT_SCRIPTS, S10
        tests = [name for name, _ in S10().variants({"tests": "dolt"})]
        self.assertEqual(len(tests), 19)
        self.assertIn("select_random_points", tests)
        self.assertLessEqual(set(DOLT_SCRIPTS), set(tests))

    def test_a_script_runs_from_the_checkout_with_its_own_tables(self):
        from pbench.workloads.sysbench import S10
        w, ctx = S10(), self._ctx("index_join")
        args = w._args(ctx, "run", [])
        self.assertEqual(args[1], "/scripts/index_join_postgres.lua")
        self.assertFalse(any(a.startswith("--tables=") for a in args))
        self.assertIn("--table-size=500", args)
        self.assertEqual(w.tables(ctx), ["sbtest1", "sbtest2"])
        builtin = self._ctx("oltp_read_write")
        self.assertEqual(w._args(builtin, "run", [])[1], "oltp_read_write")
        self.assertIn("--tables=4", w._args(builtin, "run", []))

    def test_a_script_without_the_checkout_is_unavailable(self):
        from pbench.core.interfaces import Unavailable
        from pbench.workloads.sysbench import S10
        with patch("shutil.which", return_value="/usr/bin/sysbench"), \
                self.assertRaises(Unavailable):
            S10().check(self._ctx("table_scan", scripts="/nonexistent"))

    def test_config_takes_dolt_or_known_tests(self):
        path = HERE / "configs" / "smoke.toml"
        config.load(path, {"workloads": {"S10": {"tests": "dolt"}}})
        config.load(path, {"workloads": {"S10": {"tests": ["index_scan", "oltp_insert"]}}})
        for tests in ("all", ["oltp_nope"]):
            with self.assertRaises(config.ConfigError):
                config.load(path, {"workloads": {"S10": {"tests": tests}}})


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

    def test_c_configs_may_run_without_durability(self):
        for name in ("c_standard.toml", "c_smoke.toml"):
            cfg = config.load(HERE / "configs" / name, {}, "C")
            self.assertEqual(cfg["postgres"]["fsync"], "off")
            self.assertEqual(cfg["run"]["collectors"], [])
        with self.assertRaises(config.ConfigError):
            config.load(HERE / "configs" / "c_smoke.toml", {}, "S")

    def test_fsync_off_refused(self):
        with self.assertRaises(config.ConfigError):
            config.load(HERE / "configs" / "smoke.toml", {"postgres": {"fsync": "off"}})

    def test_unknown_run_key_refused(self):
        with tempfile.NamedTemporaryFile("w", suffix=".toml") as f:
            f.write("[run]\nrepetitons = 3\n")
            f.flush()
            with self.assertRaises(config.ConfigError):
                config.load(Path(f.name), {})


class V6RangeTest(unittest.TestCase):
    def test_each_branch_changes_n_rows(self):
        from pbench.workloads.v_suite import V6
        rows, n = 100000, 30000
        for b in range(20):
            lo, past = V6.changed(b, n, rows)
            ids = [i for i in range(1, rows + 1) if lo < i <= lo + n or i <= past]
            self.assertEqual(len(ids), n, f"branch {b}")


class V8MetricsTest(unittest.TestCase):
    def test_each_stage_flat(self):
        from pbench.workloads.v_suite import V8
        files = {c: 1000 for c in V8.FILES}
        reading = {"breakdown": {**files, "heap_main_bytes": 500, "change_log_bytes": None},
                   "database_bytes": 40000, "table_bytes": 16384, "heap_bytes": 4000}
        m = V8.metrics({s: reading for s in V8.STAGES})
        for s in V8.STAGES:
            self.assertEqual(m[f"{s}_page_store_bytes"], 1000.0)
            self.assertIsNone(m[f"{s}_change_log_bytes"])   # (the log can't say)
            # (the files, not the parts of the page store)
            self.assertEqual(m[f"{s}_prolly_bytes"], 1000.0 * len(V8.FILES))
            self.assertEqual(m[f"{s}_prolly_over_heap"], 0.25 * len(V8.FILES))
        # (a file column unknown: no sum; no breakdown: the sizes still)
        m = V8.metrics({s: {**reading, "breakdown": {**files, "chunk_store_other_bytes": None}}
                        for s in V8.STAGES})
        self.assertIsNone(m["gc_prolly_bytes"])
        m = V8.metrics({s: {**reading, "breakdown": None} for s in V8.STAGES})
        self.assertEqual(m["gc_database_bytes"], 40000.0)
        self.assertNotIn("gc_page_store_bytes", m)

    def test_no_background(self):
        from pbench.workloads.v_suite import V8
        # (whatever the suite's backgrounds: a cell with one can't run)
        self.assertEqual(V8().variants({}), [("bgnone", {"background": "none"})])
        self.assertEqual(V8().variants({"background": ["S1"]}),
                         [("bgnone", {"background": "none"})])

    def test_updates_wrap(self):
        import re
        from pbench.workloads.v_suite import V8

        def ids(where, n):
            m = re.fullmatch(r"WHERE \(id > (-?\d+) AND id <= (-?\d+)\) OR id <= (-?\d+)", where)
            lo, hi, wrapped = map(int, m.groups())
            return {i for i in range(1, n + 1) if lo < i <= hi or i <= wrapped}

        # (1,000 rows, 3% a commit, 40 commits: past the end, still 30 a
        # commit, the ones after the last)
        n, changed = 1000, 30
        seen = []
        for i in range(40):
            got = ids(V8.update_where(i, changed, n), n)
            self.assertEqual(len(got), changed, i)
            seen.append(got)
        self.assertEqual(seen[33], set(range(991, 1001)) | set(range(1, 21)))


class RegistryTest(unittest.TestCase):
    def test_everything_registered(self):
        registry.load_all()
        names = registry.workloads.names()
        self.assertEqual(sorted((n for n in names if n.startswith("S")),
                                key=lambda n: int(n[1:])),
                         [f"S{i}" for i in range(1, 11)] + ["S13"])
        self.assertEqual(sorted((n for n in names if n.startswith("V")),
                                key=lambda n: int(n[1:])),
                         ["V1", "V2", "V3", "V4", "V6", "V7", "V8", "V9", "V10"])
        for _, cls in registry.workloads.items():
            self.assertTrue(issubclass(cls, Workload))
        for name, cls in registry.collectors.items():
            self.assertTrue(issubclass(cls, Collector))
            self.assertEqual(cls.name, name)
        for name in config.RUN_DEFAULTS["collectors"]:
            registry.collectors.get(name)

    def test_c_workloads(self):
        registry.load_all()
        self.assertEqual([n for n in registry.workloads.names() if n.startswith("C")],
                         [f"C{i}" for i in range(1, 5)])
        self.assertEqual(registry.workloads.get("C4")().client_counts([1, 4], {}), [1])

    def test_v_variants_include_background(self):
        registry.load_all()
        v2 = registry.workloads.get("V2")()
        labels = [label for label, _ in v2.variants({"changes": [1]})]
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
            seal(base)
            seal(other)
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
        seal(d)
        return d

    def test_engines_side_by_side(self):
        from pbench import table
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            self._set(root, "B0", "heap", 1000.0)
            self._set(root, "prolly", "prolly", 800.0)
            self._set(root, "prolly_x", "prolly", None, "not_implemented")
            self.assertEqual(len(table.discover(root, "S")), 3)
            sets = [table.load_set(root / n) for n in ("B0", "prolly", "prolly_x")]
            md = table.to_markdown(sets, "S")
            self.assertIn("| B0 (heap) | prolly | prolly_x (prolly) |", md)
            self.assertIn("| 1,000 | 800 (0.80x) | INVALID |", md)
            self.assertIn("| 2.00 | 2.00 (1.00x) | INVALID |", md)
            page = table.to_html(sets, "S")
            self.assertNotIn("class='num bad'", page)   # 0.80x throughput is worse
            self.assertIn("INVALID", page)

    def test_write_metrics_na_when_read_only(self):
        from pbench import table
        rs = [{"status": "ok", "read_only": True,
               "metrics": {"wal_bytes_per_operation": 0.01, "tps": 5.0}}]
        self.assertEqual(table._cell(rs, "wal_bytes_per_operation", None).text, "n/a")
        self.assertEqual(table._cell(rs, "throughput", None).text, "5.00")
        rs[0]["read_only"] = False
        self.assertEqual(table._cell(rs, "wal_bytes_per_operation", None).text, "0.01")


class CSuiteTest(unittest.TestCase):
    def test_targets(self):
        self.assertIsNone(targets.for_am("heap").schema)
        self.assertEqual(targets.for_am("prolly").schema, targets.VERSIONED_SCHEMA)
        self.assertEqual(targets.for_am("heap").setup_sql("bench"), "")
        sql = targets.for_am("prolly").setup_sql("bench")
        self.assertIn("CREATE SCHEMA versioned VERSIONED", sql)
        self.assertIn("ALTER DATABASE bench SET search_path = versioned, public", sql)

    def test_wrong_am(self):
        class FakeCluster:
            ams = {"t1": "prolly", "t2": "heap", "gone": ""}

            def scalar(self, sql, dbname):
                return next(am for t, am in self.ams.items() if f"'{t}'" in sql)
        got = targets.for_am("prolly").wrong_am(FakeCluster(), "bench", ["t1", "t2", "gone"])
        self.assertEqual(got, ["t2:heap", "gone:missing"])

    def test_sample_per_directory(self):
        from pbench.workloads.c_suite import C3
        with tempfile.TemporaryDirectory() as d:
            for f in ("index/between/1/a.test", "index/between/1/b.test",
                      "index/in/10/c.test", "index/in/10/d.test"):
                (Path(d) / "test" / f).parent.mkdir(parents=True, exist_ok=True)
                (Path(d) / "test" / f).write_text("")
            ctx = _ctx(tools={"sqllogictest": d}, params={"sample": "per_directory"})
            got = [Path(f).relative_to(Path(d) / "test").as_posix() for f in C3().files(ctx)]
            self.assertEqual(got, ["index/between/1/a.test", "index/in/10/c.test"])
            ctx = _ctx(tools={"sqllogictest": d}, params={})
            self.assertEqual(C3().files(ctx), [str(Path(d) / "test" / "index")])

    def test_metrics(self):
        from pbench.workloads.c_suite import metrics
        m = metrics({"total": 10, "files": 2, "pass_percent": 87.5, "reruns": [],
                     "unmatched_digests": 0,
                     "counts": {"ok": 7, "not_ok": 1, "skipped": 2}})
        self.assertEqual((m["records"], m["ok"], m["not_ok"], m["crashed"]), (10, 7, 1, 0))
        m = metrics({"total": 3, "files": 1, "pass_percent": 0, "reruns": [],
                     "unmatched_digests": 0, "counts": {"skipped": 3}})
        self.assertIsNone(m["pass_percent"])


def _ctx(*, tools, params):
    from pbench.core.interfaces import Context
    key = CellKey("C", "C3", "corpus", "", 1, 1)
    return Context(key=key, cluster=None, table_am="heap", size_params={}, params=params,
                   warmup_seconds=0, measure_seconds=0, raw_dir=Path("."),
                   scratch_dir=Path("."), tools=tools)


class CCompareTest(unittest.TestCase):
    """Rendering of comparisons slt-runner already wrote (it isn't run)."""

    def _set(self, root: Path, name: str, am: str, label: str):
        from pbench.table import ResultSet
        d = root / name
        raw = d / "raw" / "C2" / "corpus" / "c1" / "r1"
        (raw / "slt").mkdir(parents=True)
        with gzip.open(raw / "slt" / "records.jsonl.gz", "wt") as f:
            f.write("{}\n")
        (d / "config.json").write_text(json.dumps({"tools": {
            "slt_runner": "/nonexistent", "sqllogictest": str(root / "corpus")}}))
        result = {"key": {"workload": "C2"}, "status": "ok", "raw_dir": str(raw),
                  "metrics": {"records": 10, "skipped": 2, "ok": 6}}
        return ResultSet(d, "C", label, am, "2026-10-15T00:00:00", [result])

    def test_table_and_cdiff(self):
        from pbench import ccompare
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            (root / "corpus" / "test" / "evidence").mkdir(parents=True)
            (root / "corpus" / "test" / "evidence" / "in1.test").write_text(
                "query I nosort\nSELECT 1\n  FROM t1\n----\n1\n")
            base = self._set(root, "B0", "heap", "B0")
            eng = self._set(root, "eng", "prolly", "prolly")
            dest = eng.path / "compare" / "B0" / "C2"
            dest.mkdir(parents=True)
            (dest / "compare.json").write_text(json.dumps({
                "counts": {"agree": 5, "regression": 1}, "gate_failures": 1,
                "stale_exceptions": []}))
            with gzip.open(dest / "diffs.jsonl.gz", "wt") as f:
                f.write(json.dumps({"file": "evidence/in1.test", "line": 2,
                                    "class": "regression",
                                    "baseline": {"status": "ok", "rows": 1},
                                    "engine": {"status": "not_ok", "sqlstate": "XX000",
                                               "msg": "boom"}}) + "\n")
            md = ccompare.to_markdown([eng, base])
            self.assertIn("| C2 | 75.000% | 75.000% |", md)
            self.assertIn("| C2 | - | FAIL |", md)
            with patch.object(ccompare, "compare", return_value={}):
                text = ccompare.cdiff(base, eng)
            self.assertIn("## regression: 1", text)
            self.assertIn("SELECT 1\n    FROM t1", text)
            self.assertIn("prolly: not_ok, XX000, boom", text)

    def test_exceptions_for(self):
        with tempfile.NamedTemporaryFile("w", suffix=".toml") as f:
            f.write('[[exception]]\nfile = "a.test"\nline = 3\nam = "prolly"\nreason = "all"\n'
                    '[[exception]]\nfile = "b.test"\nline = 4\nam = "prolly"\nreason = "p"\n')
            f.flush()
            from pbench import ccompare
            self.assertEqual(len(ccompare.exceptions_for("prolly", Path(f.name))), 2)
            self.assertEqual(len(ccompare.exceptions_for("heap", Path(f.name))), 0)


class VSuiteSQLTest(unittest.TestCase):
    """Each engine family gets its own version-control functions: page-based
    prolly has no prolly_* ones."""

    def ctx(self, am):
        from types import SimpleNamespace
        return SimpleNamespace(table_am=am)

    def test_prolly_uses_dolt_functions(self):
        from pbench.workloads.v_suite import VC_SQL as vc
        stmts = [vc.branch("b"), vc.checkout("b"), vc.commit("m"),
                 vc.diff_count("main", "b", "vc_data"), vc.merge("b")]
        self.assertTrue(all("dolt_" in s and "prolly_" not in s for s in stmts), stmts)
        self.assertEqual(vc.commit("m"), "SELECT dolt_commit('-Am', 'm')")
