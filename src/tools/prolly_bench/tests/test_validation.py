import json
import sqlite3
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from evidence_helpers import seal
from pbench import table
from pbench.core import config
from pbench.core.interfaces import Collector
from pbench.core.runner import CollectorWindow, interleave
from pbench.core.interfaces import CellKey
from pbench.core.stats import LatencyHistogram, paired_ratio_interval, counter_delta
from pbench.core.validation import compatible, validate_run, comparison_identity
from pbench.workloads.v_suite import V3
from pbench import complexity
from pbench.core import process

HERE = Path(__file__).resolve().parents[1]


class ValidationTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.results = [{"key": {"suite": "S", "workload": "S1", "rep": 1},
                         "status": "ok", "metrics": {"tps": 100}}]
        (self.path / "results.jsonl").write_text(json.dumps(self.results[0]) + "\n")
        seal(self.path)

    def test_intact_then_changed_same_size_same_mtime(self):
        self.assertEqual(validate_run(self.path), [])
        path = self.path / "results.jsonl"
        stat = path.stat()
        path.write_text(path.read_text().replace('100', '200'))
        import os
        os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        self.assertTrue(validate_run(self.path))

    def test_incomplete_duplicate_missing_and_legacy(self):
        path = self.path / "run.json"
        original = json.loads(path.read_text())
        for change in ({"state": "running"}, {"schema_version": 1}, {"planned_cells": []},
                       {"planned_cells": original["planned_cells"] * 2}, {"artifacts": {}}):
            path.write_text(json.dumps({**original, **change}))
            self.assertTrue(validate_run(self.path), change)

    def test_failures_never_disappear_from_tables(self):
        good = {"status": "ok", "metrics": {"tps": 200}}
        bad = {"status": "error", "metrics": {"tps": 1000}}
        cell = table._cell([bad] * 4 + [good], "throughput", [good])
        self.assertFalse(cell.ok)
        self.assertEqual(cell.text, "invalid 4/5")
        self.assertIsNone(cell.ratio)
        cell = table._cell([good], "throughput", [bad, good])
        self.assertIsNone(cell.ratio)
        self.assertIsNone(table._cell([good], "throughput", [good]).better)

    def test_discovery_does_not_hide_newer_failed_or_incomplete_runs(self):
        for name, status in (("old", "ok"), ("new", "error")):
            p = self.path / name
            p.mkdir()
            (p / "results.jsonl").write_text(json.dumps({**self.results[0], "status": status}) + "\n")
            seal(p)
            manifest = json.loads((p / "run.json").read_text())
            manifest["started"] = "2026-10-16T12:00:00" if name == "old" else "2026-10-16T13:00:00"
            (p / "run.json").write_text(json.dumps(manifest))
        found = table.discover(self.path, "S")
        self.assertEqual([r.path.name for r in found], ["new"])
        self.assertTrue(found[0].problems)
        (self.path / "new/results.jsonl").write_text('{"truncated":')
        found = table.discover(self.path, "S")
        self.assertEqual([r.path.name for r in found], ["new"])
        self.assertTrue(found[0].problems)

    def test_missing_metric_is_not_an_average_over_survivors(self):
        c = table._cell([{"status": "ok", "metrics": {"tps": 2}},
                         {"status": "ok", "metrics": {}}], "throughput", None)
        self.assertFalse(c.ok)
        self.assertEqual(c.text, "missing 1/2")

    def test_different_experiments_cannot_form_ratios(self):
        other = self.path / "other"
        other.mkdir()
        (other / "results.jsonl").write_text(json.dumps(self.results[0]) + "\n")
        seal(other)
        self.assertEqual(compatible(self.path, other), [])
        p = other / "run.json"
        value = json.loads(p.read_text())
        value["comparison_identity"] = "different-scale-or-fpw"
        p.write_text(json.dumps(value))
        self.assertTrue(compatible(self.path, other))

    def test_build_location_is_not_a_build_mode_change(self):
        def identity(prefix, flags):
            return comparison_identity({}, {"postgres": {
                "configure": f"'--prefix={prefix}' '--with-openssl' {flags}", "cflags": "-O2"}})
        self.assertEqual(identity("/tmp/revision-a", ""), identity("/tmp/revision-b", ""))
        self.assertNotEqual(identity("/tmp/revision-a", ""),
                            identity("/tmp/revision-b", "'--enable-cassert'"))

    def test_zero_work_and_nonfinite_measurements_cannot_pass(self):
        for change in ({"denominator": 0}, {"denominator": True},
                       {"measurement": {"seconds": float("nan")}},
                       {"metrics": {"tps": float("inf")}}):
            p = self.path / "results.jsonl"
            p.write_text(json.dumps({**self.results[0], **change}) + "\n")
            seal(self.path)
            self.assertTrue(validate_run(self.path), change)

    def test_collector_failure_is_evidence(self):
        class Broken(Collector):
            name = "broken"
            def stop(self, ctx):
                raise RuntimeError("counter reset")
        statuses = {}
        window = CollectorWindow(None, [Broken()], statuses)
        with window:
            pass
        self.assertEqual(statuses["broken"][0], "error")
        self.assertEqual(len(window.timings["broken"]), 2)
        self.assertEqual(window.collectors, [])

    def test_counter_resets_and_missing_counters_invalidate_intervals(self):
        self.assertEqual(counter_delta({"bytes": 10}, {"bytes": 14}), {"bytes": 4})
        for after in ({}, {"bytes": 9}, {"bytes": float("nan")}):
            with self.assertRaises(ValueError):
                counter_delta({"bytes": 10}, after)

    def test_percentiles_need_observations_and_finite_inputs(self):
        h = LatencyHistogram()
        h.add(1000, 22)
        self.assertIsNone(h.reported_percentile(99))
        h.add(2000, 10000)
        self.assertIsNotNone(h.reported_percentile(99.9))
        for value in (-1, float("nan"), float("inf")):
            with self.assertRaises(ValueError):
                h.add(value)


class ConfigValidationTest(unittest.TestCase):
    def test_invalid_run_and_workload_configuration(self):
        for over in ({"run": {"repetitions": 0}}, {"run": {"clients": []}},
                     {"run": {"clients": [1, 1]}}, {"run": {"repetitions": 1.5}},
                     {"run": {"latency_sampling_rate": 2}}, {"run": {"sizes": []}},
                     {"workloads": {"C3": {"sample": "typo"}}},
                     {"workloads": {"S13": {"modes": []}}},
                     {"workloads": {"S1": {"typo": 3}}}):
            with self.subTest(over=over), self.assertRaises(config.ConfigError):
                config.load(HERE / "configs/smoke.toml", over)


class ExperimentTest(unittest.TestCase):
    def test_interleaving_is_complete_paired_and_reproducible(self):
        cells = [(CellKey("S", "S1", "tiny", "", 1, rep), {}) for rep in range(1, 6)]
        schedule = interleave([cells, cells], 7)
        self.assertEqual(schedule, interleave([cells, cells], 7))
        self.assertEqual(len(schedule), 10)
        for a, b in zip(schedule[::2], schedule[1::2]):
            self.assertEqual(a[1], b[1])
            self.assertEqual({a[0], b[0]}, {0, 1})
        self.assertGreater(len({s[0] for s in schedule[::2]}), 1)

    def test_growth_bound_and_minimum_evidence(self):
        self.assertTrue(complexity.analyze({n: [5] * 5 for n in (100, 200, 400, 800)}, .3)["passed"])
        self.assertFalse(complexity.analyze({n: [n] * 5 for n in (100, 200, 400, 800)}, .3)["passed"])
        self.assertFalse(complexity.analyze({n: [5] for n in (100, 200, 400, 800)}, .3)["passed"])
        self.assertFalse(complexity.analyze({n: [0] * 5 for n in (100, 200, 400, 800)}, .3)["passed"])

    def test_gate_without_counters_is_inconclusive(self):
        import contextlib
        import io
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / "config.json").write_text(json.dumps({"sizes": {"s": {"v_rows": 100}}}))
            results = []
            for rep in (1, 2):
                raw = path / f"raw{rep}"
                raw.mkdir()
                results.append({"key": {"suite": "V", "workload": "V2", "size": "s", "variant": "",
                                        "clients": 1, "rep": rep},
                                "status": "ok", "raw_dir": f"raw{rep}", "metrics": {}})
            (path / "results.jsonl").write_text("".join(json.dumps(r) + "\n" for r in results))
            seal(path)
            args = SimpleNamespace(dir=path, workload="V2", operation="dolt_commit",
                                   counter="nodes_read", max_exponent=.3)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(complexity.run(args), 2)
            self.assertTrue(json.loads(out.getvalue())["inconclusive"])

            # Some results with counters and some without: incomplete, a failure.
            (path / "raw1" / "v2-counters.json").write_text(json.dumps(
                [{"sql": "SELECT dolt_commit('-m', 'x')", "nodes_read": 3}]))
            seal(path)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(complexity.run(args), 1)
            self.assertNotIn("inconclusive", json.loads(out.getvalue()))

    def test_paired_intervals_and_small_samples(self):
        self.assertEqual(paired_ratio_interval([(2, 1)] * 5), (2, 2, 2))
        self.assertIsNone(paired_ratio_interval([(2, 1)] * 4))
        self.assertIsNone(paired_ratio_interval([(2, 0)] * 5))

    def test_timeout_terminates_client(self):
        import subprocess
        import sys
        with self.assertRaises(subprocess.TimeoutExpired):
            process.run([sys.executable, "-c", "import time; time.sleep(60)"], timeout=.05)

    def test_stream_hash_is_complete_and_failed_streams_are_rejected(self):
        import hashlib
        import sys
        expected = hashlib.sha256(b"x" * (3 * 1024 * 1024)).hexdigest()
        command = [sys.executable, "-c", "import sys; sys.stdout.buffer.write(b'x' * (3 * 1024 * 1024))"]
        self.assertEqual(process.hash_output(command), expected)
        with self.assertRaises(RuntimeError):
            process.hash_output([sys.executable, "-c", "print('partial'); raise SystemExit(1)"])

    def test_tpch_answers_and_parameters_are_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            base, engine = Path(tmp) / "base", Path(tmp) / "engine"
            answers = [{"version": 1, "query": f"Q{i}", "rows": 10,
                        **{k: "a" * 64 for k in ("sql_sha256", "columns_sha256", "ordered_sha256", "sorted_sha256")}}
                       for i in range(1, 23) for _ in range(2)]
            for p in (base, engine):
                (p / "raw").mkdir(parents=True)
                (p / "raw/tpch-answers.jsonl").write_text("".join(json.dumps(r) + "\n" for r in answers))
                (p / "results.jsonl").write_text(json.dumps({"key": {"workload": "S13"},
                    "status": "ok", "raw_dir": "raw", "metrics": {}}) + "\n")
                seal(p)
            self.assertEqual(compatible(base, engine), [])
            answers[0]["rows"] += 1
            path = engine / "raw/tpch-answers.jsonl"
            path.write_text("".join(json.dumps(r) + "\n" for r in answers))
            seal(engine)
            self.assertTrue(compatible(base, engine))
            path.write_text("".join(json.dumps(r) + "\n" for r in answers[:-1]))
            seal(engine)
            self.assertTrue(validate_run(engine))


class VChangeSetTest(unittest.TestCase):
    def test_exact_counts_and_overlap_using_sql(self):
        db = sqlite3.connect(":memory:")
        self.addCleanup(db.close)
        db.execute("CREATE TABLE vc_data(id integer)")
        db.executemany("INSERT INTO vc_data VALUES (?)", ((i,) for i in range(1, 10001)))
        for n, percent in ((100, 0), (100, 10), (6000, 50), (10000, 100)):
            for locality in ("clustered", "scattered"):
                ctx = SimpleNamespace(size_params={"v_rows": 10000}, params={
                    "changes": n, "conflict_percent": percent, "locality": locality})
                a, b = (V3()._ids(ctx, side) for side in (0, 1))
                for predicate, expected in ((a, n), (b, n), (f"({a}) AND ({b})", n * percent // 100)):
                    actual = db.execute(f"SELECT count(*) FROM vc_data WHERE {predicate}").fetchone()[0]
                    self.assertEqual(actual, expected, (n, percent, locality, predicate))
        ctx.params.update(changes=10000, conflict_percent=0)
        with self.assertRaises(ValueError):
            V3()._ids(ctx, 1)
