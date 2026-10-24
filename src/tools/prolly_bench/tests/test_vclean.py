"""Tests of the V suite's cleanup invariance (vclean) and its workload,
V10: a pass's script and its parsing, digests compared, latencies paired
and judged, and a run's cells reported and gated.

Run: cd src/tools/prolly_bench && python3 -m unittest discover -s tests
"""

from __future__ import annotations

import argparse
import json
import tempfile
import unittest
from pathlib import Path

from pbench import vclean
from pbench.workloads.v_suite import V10

QUERIES = [
    ("full", "read", "SELECT * FROM t"),
    ("", "set", "SET enable_seqscan = off"),
    ("checkout", "op", "SELECT dolt_checkout('side')"),
    ("side", "read", "SELECT * FROM t WHERE id < 10"),
]


def output(digests: dict[str, str], times: list[float]) -> str:
    """What psql prints for QUERIES' pass script."""
    lines = []
    ti = iter(times)
    for name, kind, _ in QUERIES:
        if kind == "set":
            continue
        lines.append(f"Time: {next(ti):.3f} ms")
        if kind == "read":
            lines.append(f"v10|{name}|{digests[name]}")
    return "\n".join(lines) + "\n"


class ScriptTest(unittest.TestCase):
    def test_reads_timed_then_digested(self):
        script = vclean.pass_script(QUERIES)
        lines = script.splitlines()
        self.assertEqual(lines[0], "\\timing on")
        # (the read is timed with its rows discarded, before its digest)
        i = lines.index("SELECT * FROM t;")
        self.assertEqual(lines[i - 1], "\\o /dev/null")
        self.assertEqual(lines[i + 1:i + 3], ["\\o", "\\timing off"])
        self.assertIn("v10|full|", lines[i + 3])
        self.assertIn("ORDER BY r::text", lines[i + 3])
        # a setting is untimed
        j = lines.index("SET enable_seqscan = off;")
        self.assertEqual((lines[j - 1], lines[j + 1]), ("\\timing off", "\\timing on"))

    def test_unknown_kind(self):
        with self.assertRaises(ValueError):
            vclean.pass_script([("x", "write", "SELECT 1")])

    def test_parse(self):
        d = {"full": "3|abc", "side": "0|"}
        digests, lat = vclean.parse_pass(output(d, [1.5, 2.0, 0.25]), QUERIES)
        self.assertEqual(digests, d)
        self.assertEqual(lat, {"full": 1.5, "checkout": 2.0, "side": 0.25})

    def test_parse_refuses_missing(self):
        with self.assertRaisesRegex(RuntimeError, "digests"):
            vclean.parse_pass("Time: 1.0 ms\nv10|full|1|a\n", QUERIES)
        with self.assertRaisesRegex(RuntimeError, "timings"):
            vclean.parse_pass(output({"full": "1|a", "side": "1|b"}, [1, 2, 3])
                              .replace("Time: 2.000 ms\n", ""), QUERIES)

    def test_mismatches(self):
        base = {"a": "1|x", "b": "2|y"}
        self.assertEqual(vclean.mismatches(base, dict(base)), [])
        self.assertEqual(vclean.mismatches(base, {"a": "1|x", "b": "2|z"}), ["b"])
        self.assertEqual(vclean.mismatches(base, {"a": "1|x"}), ["b"])


class LatencyTest(unittest.TestCase):
    def test_paired(self):
        # (pairs, not medians: repetition i against repetition i)
        self.assertAlmostEqual(vclean.paired_ratio([10, 20, 40], [11, 20, 80]), 1.1)
        with self.assertRaises(ValueError):
            vclean.paired_ratio([1, 2], [1])

    def test_judge(self):
        self.assertTrue(vclean.judge("c", "full", [10] * 5, [12] * 5).slower)
        # within the tolerance
        self.assertFalse(vclean.judge("c", "full", [10] * 5, [10.5] * 5).slower)
        # over it, but by less than the floor
        self.assertFalse(vclean.judge("c", "point", [0.2] * 5, [0.4] * 5).slower)
        self.assertEqual(vclean.judge("c", "heap_full", [1], [1]).engine, "heap")

    def test_noise(self):
        # (phase 1 moving 20% between repetitions: 1.2x after isn't slower)
        noisy = [10, 12, 8, 10, 11, 9, 10]
        self.assertGreater(vclean.tolerance_of(noisy), 1.2)
        self.assertFalse(vclean.judge("c", "full", noisy, [x * 1.2 for x in noisy]).slower)
        self.assertTrue(vclean.judge("c", "full", noisy, [x * 2 for x in noisy]).slower)
        # quiet: the fixed tolerance
        self.assertEqual(vclean.tolerance_of([10] * 5), vclean.TOLERANCE)

    def series(self, p2=12.0):
        lat1 = {"full": [10.0] * 3, "heap_full": [5.0] * 3}
        lat2 = {"full": [p2] * 3, "heap_full": [5.0] * 3}
        return {"phase1": {"latency_ms": lat1, "digests": []},
                "phase2": {"latency_ms": lat2, "digests": []}}

    def test_metrics(self):
        m = vclean.metrics(self.series())
        self.assertAlmostEqual(m["full_ratio"], 1.2)
        self.assertEqual(m["full_p1_ms"], 10.0)
        self.assertEqual(m["prolly_queries_slower"], 1.0)
        self.assertEqual(m["heap_queries_slower"], 0.0)
        self.assertAlmostEqual(m["heap_ratio_median"], 1.0)

    def run_dir(self, root: Path, status="ok", p2=12.0) -> Path:
        raw = root / "raw" / "V10"
        raw.mkdir(parents=True)
        (raw / "v10-series.json").write_text(json.dumps(self.series(p2)))
        (root / "results.jsonl").write_text(json.dumps(
            {"key": {"workload": "V10", "size": "pr", "variant": "warm-bgnone", "rep": 1},
             "status": status, "raw_dir": "raw/V10"}) + "\n")
        return root

    def args(self, d, **kw):
        return argparse.Namespace(dir=d, tolerance=vclean.TOLERANCE, floor_ms=vclean.FLOOR_MS,
                                  gate_latency=False, json=None, markdown=None, **kw)

    def test_run_reports_and_gates(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = self.run_dir(Path(tmp))
            md = Path(tmp) / "out.md"
            args = self.args(d)
            args.markdown = md
            self.assertEqual(vclean.run(args), 0)
            self.assertIn("| pr/warm-bgnone/1 | full | prolly | 10.000 | 10.000-10.000 | 12.000 "
                          "| 1.200 | 1.100 | slower |", md.read_text())
            args.gate_latency = True
            self.assertEqual(vclean.run(args), 1)

    def test_run_passes_within_tolerance(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = self.args(self.run_dir(Path(tmp), p2=10.2))
            args.gate_latency = True
            self.assertEqual(vclean.run(args), 0)

    def test_failed_cell_fails(self):
        # (a cell whose digests differed fails the run, gated or not)
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(SystemExit):
                vclean.run(self.args(self.run_dir(Path(tmp), status="error")))


class WorkloadTest(unittest.TestCase):
    def test_variants(self):
        self.assertEqual(V10().variants({}),
                         [("warm-bgnone", {"background": "none", "cache": "warm"}),
                          ("cold-bgnone", {"background": "none", "cache": "cold"})])
        with self.assertRaises(ValueError):
            V10().variants({"caches": ["lukewarm"]})

    def test_queries(self):
        q = V10().queries("c1hash", "c2hash", 20000)
        names = [n for n, kind, _ in q if kind != "set"]
        self.assertEqual(len(names), len(set(names)))
        for kind in {k for _, k, _ in q}:
            self.assertIn(kind, vclean.KINDS)
        # every current-table read the heap tables repeat, on the heap tables
        reads = {n: sql for n, kind, sql in q if kind == "read"}
        for name in V10.HEAP_READS:
            self.assertIn(f"heap_{name}", reads)
            self.assertIn("public.v10h_", reads[f"heap_{name}"])
            self.assertNotIn("public.", reads[name])
        # history at the kept commits
        self.assertIn("c1hash", reads["at_c1"])
        self.assertIn("c2hash", dict((n, s) for n, _, s in q)["reset_kept"])

    def test_cleanups(self):
        labels = [label for label, _ in V10.CLEANUPS]
        self.assertEqual(len(labels), len(set(labels)))
        sql = " ".join(s for _, s in V10.CLEANUPS if s)
        for needed in ("VACUUM (FREEZE)", "vacuum_copy_kept_pages = on", "dolt_gc('--full')",
                       "dolt_gc()", "prolly_page_sweep()", "CHECKPOINT"):
            self.assertIn(needed, sql)
        self.assertIn("release_queue", labels)

    def test_log_problems(self):
        log = "\n".join([
            "07:16:20.443 PDT [1] 00000 LOG:  checkpoint complete",
            "07:16:20.443 PDT [2] 55P03 ERROR:  another collection of this database's "
            "chunk store is running",
            "07:16:21.443 PDT [3] 57014 ERROR:  canceling autovacuum task",
            "07:16:21.500 PDT [8] 00000 LOG:  automatic vacuum of table",
            "07:16:22.000 PDT [4] 57P01 FATAL:  terminating background worker \"prolly fold "
            "launcher\" due to administrator command",
            "07:16:23.000 PDT [5] XX000 ERROR:  prolly tree node missing from its chunk store",
            "07:16:24.000 PDT [6] 01000 WARNING:  page verification failed",
            "07:16:25.000 PDT [7] 28000 FATAL:  role \"x\" does not exist"])
        # (autovacuum's errors too: V10 runs its cleanups one at a time)
        self.assertEqual([line.split("]")[0][-1] for line in V10.log_problems(log)],
                         ["2", "3", "5", "6", "7"])

    class Stubbed(V10):
        """V10's cleanups against a fake server: each cleanup grows the
        counters by grow[label], and the reads after it give digests[label]."""

        def __init__(self, grow, digests=None):
            self.grow, self.digests, self.now, self.ran = grow, digests or {}, {}, []

        def rows(self, ctx):
            return 20000

        def _counters(self, ctx):
            return dict(self.now)

        def _sql(self, ctx, sql):
            label = next(label for label, s in V10.CLEANUPS if s == sql)
            self._grew(label)
            return "0|0|0|0" if label == "page_sweep" else ""

        def _drain_release_queue(self, ctx):
            self._grew("release_queue")

        def _grew(self, label):
            self.ran.append(label)
            for c, v in self.grow.get(label, {}).items():
                self.now[c] = self.now.get(c, 0) + v

        def _churn(self, ctx, n):
            pass

        def _pass(self, ctx, queries, tag):
            return self.digests.get(self.ran[-1], {"q": "1|a"}), {}

    class Ctx:
        def __init__(self, raw):
            self.raw_dir = raw

    FREES = {"gc_full": {"gc_chunks_freed": 10}, "release_queue": {"bmap_release_blocks_freed": 5}}

    def test_cleanups_recorded(self):
        with tempfile.TemporaryDirectory() as tmp:
            w = self.Stubbed(self.FREES)
            out, record = w.cleanup(self.Ctx(Path(tmp)), [], {"q": "1|a"})
            self.assertEqual(w.ran, [label for label, _ in V10.CLEANUPS])
            self.assertEqual(out["cleanup_gc_full_gc_chunks_freed"], 10.0)
            self.assertEqual(out["cleanup_vacuum_gc_chunks_freed"], 0.0)
            self.assertEqual(out["cleanup_page_sweep_moved"], 0.0)
            self.assertEqual(record["release_queue"]["counters"]["bmap_release_blocks_freed"], 5)
            self.assertTrue((Path(tmp) / "v10-cleanups.json").exists())

    def test_cleanup_freeing_nothing_fails(self):
        for label in ("gc_full", "release_queue"):
            frees = {k: v for k, v in self.FREES.items() if k != label}
            with tempfile.TemporaryDirectory() as tmp:
                with self.assertRaisesRegex(RuntimeError, f"cleanup {label} freed nothing"):
                    self.Stubbed(frees).cleanup(self.Ctx(Path(tmp)), [], {"q": "1|a"})

    def test_reads_checked_after_each_cleanup(self):
        # (a read that differs right after the drain fails there)
        with tempfile.TemporaryDirectory() as tmp:
            w = self.Stubbed(self.FREES, {"release_queue": {"q": "1|b"}})
            with self.assertRaisesRegex(RuntimeError, "after cleanup release_queue.*q"):
                w.cleanup(self.Ctx(Path(tmp)), [], {"q": "1|a"})

    def test_sizes(self):
        self.assertEqual(V10.sizes(20000)["big"], 100)
        with self.assertRaises(ValueError):
            V10.sizes(100)


if __name__ == "__main__":
    unittest.main()
