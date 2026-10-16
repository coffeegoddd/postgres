"""BenchBase configuration, result validation and TPC-H scheduling."""

import csv
import json
import tempfile
import unittest
import xml.etree.ElementTree as ET
from contextlib import nullcontext
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from pbench.core.interfaces import CellKey, Context
from pbench.workloads import _benchbase as bb
from pbench.workloads.tpch import S13


class MeasuredWindow(nullcontext):
    seconds = 2


class BenchBaseTest(unittest.TestCase):
    def setUp(self):
        self.output = ""
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        raw, scratch = root / "raw", root / "scratch"
        raw.mkdir()
        scratch.mkdir()
        cluster = SimpleNamespace(jdbc_url=lambda db, options:
            f"jdbc:postgresql://localhost:5432/{db}?options=-c%20"
            f"default_table_access_method={options['default_table_access_method']}")
        self.ctx = Context(CellKey("S", "S13", "tiny", "serial", 1, 1), cluster,
                           "prolly", {"tpch_scale": 0.01}, {"mode": "serial"},
                           1, 5, raw, scratch, {})

    def test_tpch_config_keeps_fractional_scale_and_all_queries(self):
        xml = ET.parse(bb._config(self.ctx, "tpch", 0.01, 5, serial=True))
        self.assertEqual(xml.findtext("scalefactor"), "0.01")
        self.assertEqual(xml.findtext("works/work/time"), "0")
        self.assertEqual(xml.findtext("works/work/serial"), "true")
        self.assertEqual([t.text for t in xml.findall("transactiontypes/transactiontype/name")],
                         [f"Q{i}" for i in range(1, 23)])
        self.assertEqual(len(xml.findtext("works/work/weights").split(",")), 22)
        self.assertIn("default_table_access_method=prolly", xml.findtext("url"))
        xml = ET.parse(bb._config(self.ctx, "tpch", 0.01, 5))
        self.assertEqual(xml.findtext("works/work/time"), "5")
        self.assertEqual(xml.findtext("works/work/serial"), "false")

    def test_tpcc_config_preserves_mix_and_clients(self):
        self.ctx.key.clients = 4
        xml = ET.parse(bb._config(self.ctx, "tpcc", 1, 5))
        self.assertEqual(xml.findtext("terminals"), "4")
        self.assertEqual(xml.findtext("works/work/weights"), "45,43,4,4,4")
        self.assertEqual(xml.findtext("works/work/time"), "5")
        self.assertEqual(len(xml.findall("transactiontypes/transactiontype")), 5)

    def test_invalid_scale(self):
        for scale in (0, -1, float("nan"), float("inf")):
            with self.subTest(scale=scale), self.assertRaises(ValueError):
                bb._config(self.ctx, "tpch", scale, 5)

    def test_modes_use_one_worker_for_q15_shared_view(self):
        w = S13()
        for _, variant in w.variants({"modes": ["serial", "throughput"]}):
            self.assertEqual(w.client_counts([4, 16], variant), [1])
        for modes in ([], ["typo"]):
            with self.assertRaises(ValueError):
                w.variants({"modes": modes})
        self.assertEqual(w.scale(self.ctx), 0.01)
        self.assertEqual(len(w.tables(self.ctx)), 8)

    def write_results(self, ctx, bench, cfg, out_dir, **kwargs):
        out_dir.mkdir(parents=True, exist_ok=True)
        (out_dir / "test.summary.json").write_text(json.dumps({
            "Final State": "EXIT", "Measured Requests": len(self.names),
            "Throughput (requests/second)": 11, "Goodput (requests/second)": 11}))
        (out_dir / "transactions.json").write_text(json.dumps({
            kind: {"NUM_SAMPLES": count} for kind, count in {
                "completed": 2 * len(self.names), "aborted": 0, "rejected": 0,
                "unexpected": self.errors}.items()}))
        with (out_dir / "test.raw.csv").open("w") as f:
            writer = csv.writer(f)
            writer.writerow(["Transaction Name", "Latency (microseconds)"])
            writer.writerows((name, 1000) for name in self.names)
        return self.output

    def test_serial_results_require_every_query_and_keep_timings(self):
        self.names, self.errors = [f"Q{i}" for i in range(1, 23)], 0
        with patch.object(bb, "_run", side_effect=self.write_results):
            out = S13().measure(self.ctx, MeasuredWindow())
        self.assertEqual(out.unit, "query")
        self.assertEqual(out.transactions, 44)
        self.assertIsNone(out.metrics["qps"])
        self.assertEqual(out.metrics["hot_queries"], 22)
        self.assertEqual(out.metrics["client_process_qps"], 22)
        self.assertIsNone(out.metrics["latency_p99_ms"])
        self.assertFalse(out.read_only)  # Q15 writes catalogs
        timings = json.loads((self.ctx.raw_dir / "benchbase-measure/queries.json").read_text())
        self.assertEqual(timings["Q22"], {"count": 1, "mean_ms": 1, "total_ms": 1})
        self.assertFalse(list(self.ctx.scratch_dir.rglob("*.raw.csv")))

    def test_missing_duplicate_and_failed_queries_are_rejected(self):
        names = [f"Q{i}" for i in range(1, 23)]
        for selected, errors in ((names[:-1], 0), (names[:-1] + ["Q1"], 0), (names, 1)):
            self.names, self.errors = selected, errors
            with self.subTest(selected=selected, errors=errors):
                with patch.object(bb, "_run", side_effect=self.write_results):
                    with self.assertRaises(RuntimeError):
                        S13().measure(self.ctx, nullcontext())

    def test_zero_exit_with_logged_loader_error_is_rejected(self):
        result = SimpleNamespace(returncode=0, stdout="[ERROR] loader SQL failed", stderr="")
        with patch.object(bb.process, "run", return_value=result):
            with self.assertRaisesRegex(RuntimeError, "logged errors"):
                bb.load(self.ctx, "tpch", scalefactor=0.01)

    def test_retryable_tpcc_sql_errors_are_left_to_histogram_validation(self):
        result = SimpleNamespace(returncode=0,
            stdout="[WARN] Retrying transaction\nPSQLException: ERROR: deadlock detected", stderr="")
        with patch.object(bb.process, "run", return_value=result):
            out = bb._run(self.ctx, "tpcc", self.ctx.raw_dir / "config.xml",
                          self.ctx.scratch_dir, create=False, load=False,
                          execute=True, tag="measure")
        self.assertIn("deadlock detected", out)

    def test_error_outside_log_level_does_not_fail_load(self):
        # (the word in a path on the command line, or in a driver's message)
        self.ctx.tools["benchbase"] = str(self.ctx.raw_dir / "ERROR-repro")
        result = SimpleNamespace(returncode=0,
            stdout="[INFO ] retrying after PSQLException: ERROR: could not serialize", stderr="")
        with patch.object(bb.process, "run", return_value=result):
            bb.load(self.ctx, "tpch", scalefactor=0.01)

    def test_serial_warmup_is_bounded_by_warmup_seconds(self):
        seen = []

        def run(ctx, bench, cfg, out_dir, **kwargs):
            seen.append(cfg.read_text())
            raise RuntimeError("stop")
        with patch.object(bb, "_run", side_effect=run):
            with self.assertRaises(RuntimeError):
                S13().warmup(self.ctx)
        self.assertIn("<serial>false</serial>", seen[0])
        self.assertIn(f"<time>{self.ctx.warmup_seconds}</time>", seen[0])

    def test_logged_deadlocks_may_outnumber_counted_ones(self):
        # (one after the measurement ended, or logged twice)
        self.names, self.errors = ["NewOrder"], 1
        self.output = "will not be retried... sql state [40P01], error code [0].\n" * 3
        with patch.object(bb, "_run", side_effect=self.write_results):
            out = bb.execute(self.ctx, "tpcc", nullcontext(), scalefactor=1)
        self.assertEqual(out.metrics["failed_transactions"], 1)

    def test_tpcc_deadlocks_count_as_failures_but_other_sql_errors_fail_cell(self):
        self.names, self.errors = ["NewOrder"], 1
        self.output = "will not be retried... sql state [40P01], error code [0]."
        with patch.object(bb, "_run", side_effect=self.write_results):
            out = bb.execute(self.ctx, "tpcc", nullcontext(), scalefactor=1)
            self.assertEqual(out.metrics["failed_transactions"], 1)
            self.assertEqual(out.transactions, 2)
            self.assertEqual(out.metrics["measured_requests"], 1)
            self.assertEqual(out.metrics["client_reported_goodput_tps"], 11)
            self.assertAlmostEqual(out.metrics["tps"] * out.metrics["client_process_seconds"], 2)
            self.output = "will not be retried... sql state [XX000], error code [0]."
            with self.assertRaisesRegex(RuntimeError, "unexpected SQL errors"):
                bb.execute(self.ctx, "tpcc", nullcontext(), scalefactor=1)


    def test_more_queries_per_second_is_better(self):
        # (S13's headline metric: a comparison colors more as better)
        from pbench import report
        self.assertIn("qps", report.HIGHER_IS_BETTER)


    def test_correctness_pass_is_serial_in_either_mode(self):
        self.ctx.params["mode"] = "throughput"
        with patch.object(bb, "_run") as run, \
                patch("pbench.core.validation.tpch_answers"):
            S13().verify(self.ctx)
        xml = ET.parse(run.call_args.args[2])
        self.assertEqual(xml.findtext("works/work/serial"), "true")


if __name__ == "__main__":
    unittest.main()
