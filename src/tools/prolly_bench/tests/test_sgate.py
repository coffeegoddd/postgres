"""Tests of the S suite's gate (sgate) and its comment (sreport), from
fixtures: regression, improvement, accepted, unstable in the base and in
the candidate only, transient, confirmed, inconclusive, and a missing job.

Run: cd src/tools/prolly_bench && python3 -m unittest discover -s tests
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from evidence_helpers import seal
from pbench import scounters, sgate, sreport
from test_s_suite import build_set
from test_scalibrate import counter_run

BASE = {"wal_bytes": 100000.0, "io_extends": 40.0, "engine.nodes_written": 500.0,
        "io_hits": 9000.0}


class CountersGateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.t = scounters.thresholds()

    def tearDown(self):
        self.tmp.cleanup()

    def gate(self, base, cand, *, base2=None, cand2=None, accept=()):
        runs = [counter_run(self.root, n, c) for n, c in
                (("b", base), ("b2", base2 or base), ("c", cand), ("c2", cand2 or cand))]
        return sgate.gate_counters(*runs, list(accept), self.t)

    def results(self, findings):
        return {(f.counter, f.result) for f in findings}

    def test_same_passes(self):
        self.assertEqual(self.gate(BASE, BASE), [])

    def test_regression(self):
        f = self.gate(BASE, {**BASE, "wal_bytes": 120000.0})
        self.assertEqual(self.results(f), {("wal_bytes", "regression")})
        self.assertTrue(f[0].fails)

    def test_under_the_floor_or_the_percentage(self):
        # +5 events is 12% but under the 8-event floor; +100 hits is 1%
        f = self.gate(BASE, {**BASE, "io_extends": 45.0, "io_hits": 9100.0})
        self.assertEqual(f, [])

    def test_improvement_never_fails(self):
        f = self.gate(BASE, {**BASE, "engine.nodes_written": 300.0})
        self.assertEqual(self.results(f), {("engine.nodes_written", "improvement")})
        self.assertFalse(f[0].fails)

    def test_accepted(self):
        accept = [sgate.Acceptance("S1", "prolly", "wal_bytes", "logs the fold root")]
        f = self.gate(BASE, {**BASE, "wal_bytes": 120000.0}, accept=accept)
        self.assertEqual(self.results(f), {("wal_bytes", "accepted")})
        self.assertEqual(f[0].reason, "logs the fold root")
        self.assertFalse(f[0].fails)

    def test_unstable_in_base_is_reported(self):
        f = self.gate(BASE, {**BASE, "io_extends": 80.0}, base2={**BASE, "io_extends": 50.0},
                      cand2={**BASE, "io_extends": 80.0})
        self.assertEqual(self.results(f), {("io_extends", "unstable_base")})
        self.assertFalse(f[0].fails)

    def test_unstable_in_candidate_only_fails(self):
        f = self.gate(BASE, BASE, cand2={**BASE, "io_extends": 60.0})
        self.assertEqual(self.results(f), {("io_extends", "unstable_candidate")})
        self.assertTrue(f[0].fails)

    def test_a_collector_lost_in_the_candidate_fails(self):
        # pg_walinspect missing from the candidate's install: write_amp
        # unavailable, its counters gone; skipped as "one build has" they'd pass
        wa = {**BASE, "wa.logical_bytes": 470696.0, "wa.wal.total_bytes": 17146024.0,
              "wa.tree.file_bytes": 0.0}
        lost = {"write_amp": ["unavailable", "pg_walinspect missing"]}
        runs = [counter_run(self.root, "b", wa), counter_run(self.root, "b2", wa),
                counter_run(self.root, "c", BASE, collector_status=lost),
                counter_run(self.root, "c2", BASE, collector_status=lost)]
        f = sgate.gate_counters(*runs, [], self.t)
        self.assertEqual({(x.counter, x.result) for x in f}, {("collector write_amp", "invariant")})
        self.assertTrue(all(x.fails for x in f))
        self.assertIn("unavailable in the candidate: pg_walinspect missing", f[0].detail)

    def test_a_collector_unavailable_in_both_builds_is_not_a_loss(self):
        lost = {"write_amp": ["unavailable", "pg_walinspect missing"]}
        runs = [counter_run(self.root, n, BASE, collector_status=lost)
                for n in ("b", "b2", "c", "c2")]
        self.assertEqual(sgate.gate_counters(*runs, [], self.t), [])

    def test_jitter_is_listed_not_failed(self):
        # (a page more in one of the candidate's runs, as hosted runners do)
        f = self.gate(BASE, BASE, cand2={**BASE, "io_extends": 41.0})
        self.assertEqual(self.results(f), {("io_extends", "jitter")})
        self.assertFalse(f[0].fails)
        text, passed = sreport.render(
            {"thresholds": scounters.thresholds(), "passed": True, "accepted": [],
             "findings": [vars(x) for x in f]},
            None, head="c", base="b", runner="x")
        self.assertTrue(passed)
        self.assertIn("by less than a change (1)", text)

    def test_different_work_is_an_invariant(self):
        runs = [counter_run(self.root, n, BASE) for n in ("b", "b2", "c")]
        c2 = counter_run(self.root, "c2", BASE)
        res = c2 / "x-S-prolly" / "results.jsonl"
        r = json.loads(res.read_text())
        r["status"] = "error"
        res.write_text(json.dumps(r) + "\n")
        f = sgate.gate_counters(*runs, c2, [], self.t)
        self.assertTrue(any(x.result == "invariant" for x in f))

    def test_only_entries_the_diff_adds_count(self):
        with tempfile.TemporaryDirectory() as d:
            head, base = Path(d) / "head.toml", Path(d) / "base.toml"
            old = ('[[accept]]\nworkload = "S1"\nengine = "prolly"\ncounter = "wal_bytes"\n'
                   'reason = "an old trade"\n')
            new = ('[[accept]]\nworkload = "S2"\nengine = "heap"\ncounter = "io_hits"\n'
                   'reason = "a new trade"\n')
            base.write_text(old)
            head.write_text(old + new)
            got = sgate.added(sgate.load_accept(head), sgate.load_accept(base))
            self.assertEqual([(a.workload, a.counter) for a in got], [("S2", "io_hits")])
            # the old entry no longer accepts a regression
            f = self.gate(BASE, {**BASE, "wal_bytes": 120000.0}, accept=got)
            self.assertEqual(self.results(f), {("wal_bytes", "regression")})

    def test_work_differing_within_a_build_is_an_invariant(self):
        runs = [counter_run(self.root, n, BASE) for n in ("b", "b2", "c")]
        c2 = counter_run(self.root, "c2", BASE)
        res = c2 / "x-S-prolly" / "results.jsonl"
        r = json.loads(res.read_text())
        r["denominator"] = 1990
        res.write_text(json.dumps(r) + "\n")
        seal(c2 / "x-S-prolly")
        f = sgate.gate_counters(*runs, c2, [], self.t)
        self.assertEqual({(x.counter, x.result) for x in f}, {("units of work", "invariant")})

    def test_accept_needs_the_base_file(self):
        args = type("A", (), dict(accept=Path("x.toml"), accept_base=None))
        with self.assertRaises(SystemExit):
            sgate._acceptances(args)
        args.accept_base = Path("/nonexistent/base.toml")
        with self.assertRaises(SystemExit):
            sgate._acceptances(args)

    def test_json_is_strict(self):
        self.assertEqual(sgate._finite({"a": [float("inf"), 1.0], "b": float("nan")}),
                         {"a": [None, 1.0], "b": None})

    def test_an_entry_needs_a_reason(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "a.toml"
            p.write_text('[[accept]]\nworkload = "S1"\nengine = "prolly"\ncounter = "x"\nreason = " "\n')
            with self.assertRaises(ValueError):
                sgate.load_accept(p)

    def test_shipped_accept_file_loads(self):
        here = Path(__file__).resolve().parents[1]
        self.assertIsInstance(sgate.load_accept(here / "configs" / "perf_accept.toml"), list)


class TimingGateTest(unittest.TestCase):
    NOISE = [1000.0, 1010.0, 990.0, 1005.0, 995.0]

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.t = scounters.thresholds()

    def tearDown(self):
        self.tmp.cleanup()

    def run_dir(self, name, factor):
        d = self.root / name
        d.mkdir()
        build_set(d, "base", "heap", self.NOISE)
        build_set(d, "candidate", "heap", [v * factor for v in self.NOISE])
        return d

    def tps(self, cells):
        return next(c for c in cells if c.metric == "throughput")

    def test_confirmed(self):
        cells = sgate.gate_timing(self.run_dir("r1", 0.8), [self.run_dir("r2", 0.8),
                                                            self.run_dir("r3", 0.8)], self.t, [])
        c = self.tps(cells)
        self.assertEqual(c.result, "confirmed")
        self.assertEqual(c.attempts, ["regressed"] * 3)
        self.assertEqual(sgate.regressed_workloads(cells), ["S1"])

    def test_transient(self):
        cells = sgate.gate_timing(self.run_dir("r1", 0.8), [self.run_dir("r2", 1.0),
                                                            self.run_dir("r3", 0.8)], self.t, [])
        c = self.tps(cells)
        self.assertEqual(c.result, "transient")
        self.assertEqual(c.attempts, ["regressed", "same", "regressed"])

    def test_inconclusive_straddles(self):
        d = self.root / "r1"
        d.mkdir()
        build_set(d, "base", "heap", self.NOISE)
        build_set(d, "candidate", "heap", [800.0, 1200.0, 900.0, 1100.0, 940.0])
        self.assertEqual(self.tps(sgate.gate_timing(d, [], self.t, [])).result, "inconclusive")

    def test_failed_cells_and_missing_builds_are_invalid(self):
        d = self.root / "r1"
        d.mkdir()
        build_set(d, "base", "heap", self.NOISE)
        cells = sgate.gate_timing(d, [], self.t, [])          # no candidate at all
        self.assertEqual([c.result for c in cells], ["invalid"])
        d2 = self.root / "r2"
        d2.mkdir()
        build_set(d2, "base", "heap", self.NOISE)
        build_set(d2, "candidate", "heap", self.NOISE, seeds={2: 99})
        self.assertIn("invalid", {c.result for c in sgate.gate_timing(d2, [], self.t, [])})

    def test_same(self):
        self.assertEqual(self.tps(sgate.gate_timing(self.run_dir("r1", 1.0), [], self.t, [])).result,
                         "same")


class ReportTest(unittest.TestCase):
    def counters(self, findings, passed):
        return {"layer": "counters", "thresholds": scounters.thresholds(), "passed": passed,
                "findings": findings, "accepted": []}

    def finding(self, result, counter="wal_bytes", **kw):
        f = {"workload": "S1", "engine": "prolly", "counter": counter, "result": result,
             "base": 1000.0, "candidate": 1200.0, "units": 10.0, "unit": "transaction",
             "detail": "", "reason": ""}
        f.update(kw)
        return f

    def render(self, counters, timing=None, failed=()):
        return sreport.render(counters, timing, head="c" * 40, base="b" * 40,
                              runner="ubuntu-24.04", failed_jobs=list(failed),
                              reproduce="prolly_bench run S ...")

    def test_pass_with_timing_reported(self):
        timing = {"report_only": True, "thresholds": scounters.thresholds(), "cells": [
            {"workload": "S2", "clients": 8, "engine": "prolly", "metric": "throughput",
             "result": "confirmed", "ratio": 0.8, "low": 0.78, "high": 0.82,
             "over_heap": {"base": 0.71, "candidate": 0.70}, "attempts": ["regressed"] * 3,
             "problems": [], "reason": ""}]}
        text, passed = self.render(self.counters([self.finding("improvement")], True), timing)
        self.assertTrue(passed)
        self.assertTrue(text.startswith(sreport.MARKER + "\n## S suite: PASS (counters; timing reported)"))
        self.assertIn("| S2 | 8 | prolly | 0.80 [0.78, 0.82] | - | - | 0.71 → 0.70 | confirmed |", text)
        self.assertIn("<summary>Improvements (1)</summary>", text)

    def test_regression_fails(self):
        text, passed = self.render(self.counters([self.finding("regression")], False))
        self.assertFalse(passed)
        self.assertIn("| S1 | prolly | wal_bytes (per txn) | 100 | 120 | +20.0% | +200 B | **FAIL** |", text)

    def test_accepted_listed_with_reason(self):
        text, passed = self.render(self.counters(
            [self.finding("accepted", reason="fix #210 logs the fold root")], True))
        self.assertTrue(passed)
        self.assertIn('accepted: "fix #210 logs the fold root"', text)
        self.assertIn("### Accepted regressions", text)

    def test_unstable_base_collapsed(self):
        text, passed = self.render(self.counters([self.finding("unstable_base")], True))
        self.assertTrue(passed)
        self.assertIn("<summary>Unstable counters in the base (1; reported, not gated)</summary>", text)

    def test_missing_job_fails(self):
        text, passed = self.render(self.counters([], True), failed=["counters (S1-S4): failure"])
        self.assertFalse(passed)
        self.assertIn("**counters (S1-S4): failure**", text)
        text, passed = self.render(None)
        self.assertFalse(passed)
        self.assertIn("No counter results", text)

    def test_length_cap(self):
        many = [self.finding("regression", counter=f"engine.c{i}") for i in range(2000)]
        text, _ = self.render(self.counters(many, False))
        self.assertLessEqual(len(text), sreport.LIMIT)
        self.assertTrue(text.startswith(sreport.MARKER))

    def test_cut_leaves_no_section_open(self):
        jitter = [self.finding("jitter", counter=f"engine.c{i}") for i in range(2000)]
        text, passed = self.render(self.counters(jitter, True))
        self.assertTrue(passed)
        self.assertLessEqual(len(text), sreport.LIMIT)
        self.assertEqual(text.count("<details>"), text.count("</details>"))
        self.assertEqual(text.count("```") % 2, 0)
        self.assertIn("cut at 65,000 characters", text)

    def test_cells_are_escaped(self):
        text, _ = self.render(self.counters(
            [self.finding("accepted", reason="fold root | see\n#210")], True))
        self.assertIn('accepted: "fold root \\| see #210"', text)

    def test_missing_results_files_fail(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "comment.md"
            args = type("A", (), dict(counters=[Path(d) / "nope.json"], timing=Path(d) / "t.json",
                                      failed_job=[], head="c", base="b", runner="x",
                                      run_url=None, reproduce=None, out=out))
            self.assertEqual(sreport.run(args), 1)
            text = out.read_text()
            self.assertIn("counter results missing", text)
            self.assertIn("timing results missing", text)

    def test_invalid_timing_fails_even_reported(self):
        timing = {"report_only": True, "thresholds": scounters.thresholds(), "cells": [
            {"workload": "S1", "clients": 8, "engine": "heap", "metric": "-",
             "result": "invalid", "ratio": None, "low": None, "high": None, "over_heap": {},
             "attempts": [], "problems": ["repetition 3 invalid"], "reason": ""}]}
        text, passed = self.render(self.counters([], True), timing)
        self.assertFalse(passed)
        self.assertIn("**invalid**: repetition 3 invalid", text)


if __name__ == "__main__":
    unittest.main()
