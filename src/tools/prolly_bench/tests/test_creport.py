import gzip
import json
import tempfile
import unittest
from pathlib import Path

from pbench import cbaseline, ccompare, creport
from evidence_helpers import c_result

FILES = {"C1": [f"select{i}.test" for i in range(1, 6)], "C2": ["evidence/e.test"],
         "C3": ["index/a.test"], "C4": ["random/r.test"]}
SHARDS = {"1/2": ["C1", "C2"], "2/2": ["C3", "C4"]}
FAILING = ("index/a.test", 9)


class ReportTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.corpus = self.root / "corpus" / "test"
        for name in sum(FILES.values(), []):
            (self.corpus / name).parent.mkdir(parents=True, exist_ok=True)
            (self.corpus / name).write_text(
                "\n".join(f"SELECT {n} FROM t{n}" if n % 4 == 1 else "" for n in range(1, 21)))
        self.pin = self.root / "pin"
        self.pin.write_text("pin\n")
        self.baseline = self.root / "baseline.tsv.gz"
        cbaseline.write(self.baseline, "pin", {FAILING: ("not_ok", "42883", "-")})
        self.base_baseline = self.root / "base.tsv.gz"
        cbaseline.write(self.base_baseline, "pin", {FAILING: ("not_ok", "42883", "-")})
        # Per shard and engine: workload -> records; per shard: the comparison.
        self.records = {}
        for spec, ws in SHARDS.items():
            for am in ("heap", "prolly"):
                self.records[spec, am] = {w: [{"file": f, "line": line, "status": "ok"}
                                              for f in FILES[w] for line in (1, 5, 9)]
                                          for w in ws}
                for w in ws:
                    for r in self.records[spec, am][w]:
                        if (r["file"], r["line"]) == FAILING:
                            r.update(status="not_ok", sqlstate="42883")
        self.compare = {spec: {w: {"counts": {"agree": 3 * len(FILES[w])}, "gate_failures": 0,
                                   "stale_exceptions": []} for w in ws}
                        for spec, ws in SHARDS.items()}
        self.compare["2/2"]["C3"]["counts"] = {"agree": 2, "postgres_semantics": 1}
        self.diffs = {spec: {w: [] for w in ws} for spec, ws in SHARDS.items()}
        self.skip = set()

    def sets(self):
        self.n = getattr(self, "n", 0) + 1
        dirs = []
        heaps = {}
        for (spec, am), recs in self.records.items():
            if (spec, am) in self.skip:
                continue
            path = self.root / f"results{self.n}" / spec.replace("/", "of") / f"2026-10-18-C-{am}"
            ws = SHARDS[spec]
            config = {"run": {"shard": spec, "workloads": ws},
                      "workloads": {w: {"shard_files": FILES[w]} for w in ws},
                      "tools": {"sqllogictest": str(self.corpus.parent),
                                "slt_runner": str(self.root / "no-runner")}}
            rs = c_result(path, am, recs, config)
            heaps[spec] = heaps.get(spec, rs)
            if am == "prolly" and (spec, "heap") not in self.skip:
                for w in ws:
                    d = path / "compare" / "2026-10-18-C-heap" / w
                    d.mkdir(parents=True)
                    (d / "compare.json").write_text(json.dumps(self.compare[spec][w]))
                    with gzip.open(d / "diffs.jsonl.gz", "wt") as f:
                        f.writelines(json.dumps(x) + "\n" for x in self.diffs[spec][w])
                    # (as cgate leaves it in the shard's job)
                    ccompare.seal_comparison(heaps[spec], rs, w)
            dirs.append(path)
        return dirs

    def report(self, **kwargs):
        return creport.build(self.sets(), 2, self.base_baseline, baseline=self.baseline,
                             pin_file=self.pin, corpus=self.corpus, head="abc123",
                             base="def456", run_url="https://ci/run/1", wall_clock="9 min",
                             **kwargs)

    def row(self, text, label):
        return next(line for line in text.splitlines() if line.startswith(f"| {label} |"))

    def test_pass(self):
        r = self.report()
        self.assertTrue(r.passed, r.problems)
        lines = r.text.splitlines()
        self.assertEqual(lines[0], creport.MARKER)
        self.assertEqual(lines[1], "## C suite: sqllogictest @ pin, full corpus: PASS")
        self.assertEqual(lines[3], "Merge of abc123 into master def456 · 24 records · 2 shards · 9 min")
        self.assertIn("| 95.833% | ±0 | — | 1 |", self.row(r.text, "B0"))
        self.assertTrue(self.row(r.text, "B0").endswith("| baseline: 0 new, 0 fixed |"))
        self.assertEqual(self.row(r.text, "prolly"),
                         "| prolly | 95.833% | ±0 | 23 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | pass |")
        self.assertIn("<summary>New failures (0)</summary>", r.text)
        self.assertIn("<summary>Fixed (0): baseline entries to remove</summary>", r.text)
        self.assertIn("<summary>Stale exceptions (0)</summary>", r.text)
        self.assertIn("| 2/2 | C3 | 2 s | 2 s |", r.text)
        self.assertIn("[artifacts](https://ci/run/1)", r.text)
        self.assertTrue(r.summary["pass"])

    def test_engine_regression(self):
        self.records["2/2", "prolly"]["C4"][0].update(status="not_ok", sqlstate="XX000",
                                                      msg="prolly: lost a row")
        self.compare["2/2"]["C4"] = {"counts": {"agree": 2, "regression": 1}, "gate_failures": 1,
                                     "stale_exceptions": []}
        self.diffs["2/2"]["C4"] = [{
            "file": "random/r.test", "line": 1, "class": "regression",
            "baseline": {"status": "ok", "rows": 1, "s": "a"},
            "engine": {"status": "not_ok", "sqlstate": "XX000", "msg": "prolly: lost a row"}}]
        r = self.report()
        self.assertFalse(r.passed)
        self.assertTrue(r.text.splitlines()[1].endswith(": FAIL"))
        self.assertTrue(self.row(r.text, "prolly").endswith("| 1 | 0 | 0 | 0 | 0 | 0 | FAIL |"))
        self.assertIn("<summary>New failures (1)</summary>", r.text)
        self.assertIn("#### regression: 1", r.text)
        self.assertIn("- `random/r.test:1` (prolly, shard 2/2)", r.text)
        self.assertIn("  - query: `SELECT 1 FROM t1`", r.text)
        self.assertIn("  - heap: ok, 1 rows", r.text)
        self.assertIn("  - prolly: not_ok, XX000, prolly: lost a row", r.text)
        self.assertIn("| -4.167 pp (+1 failing) |", self.row(r.text, "prolly"))

    def test_heap_regression(self):
        for am in ("heap", "prolly"):
            self.records["1/2", am]["C1"][0].update(status="not_ok", sqlstate="22012")
        self.compare["1/2"]["C1"]["counts"] = {"agree": 14, "postgres_semantics": 1}
        r = self.report()
        self.assertFalse(r.passed)
        self.assertIn("baseline: 1 new, 0 fixed (FAIL)", self.row(r.text, "B0"))
        self.assertIn("#### heap regression: 1", r.text)
        self.assertIn("- `select1.test:1` (heap)", r.text)
        self.assertIn("  - baseline: passes", r.text)
        self.assertIn("  - heap: not_ok 22012 -", r.text)
        self.assertIn("-4.167 pp (+1 failing)", self.row(r.text, "B0"))
        self.assertTrue(self.row(r.text, "prolly").endswith("| pass |"))

    def test_fixed_records(self):
        for am in ("heap", "prolly"):
            for rec in self.records["2/2", am]["C3"]:
                rec.update(status="ok")
                rec.pop("sqlstate", None)
        self.compare["2/2"]["C3"]["counts"] = {"agree": 3}
        r = self.report()
        self.assertFalse(r.passed)
        self.assertIn("baseline: 0 new, 1 fixed (FAIL)", self.row(r.text, "B0"))
        self.assertIn("<summary>Fixed (1): baseline entries to remove</summary>", r.text)
        self.assertIn("- `index/a.test:9` (was not_ok 42883 -)", r.text)
        self.assertIn("+4.167 pp (-1 failing)", self.row(r.text, "B0"))

    def test_change_is_against_the_base_baseline(self):
        # (the base listed one more failure, which this pull request fixed
        # and removed from the file)
        cbaseline.write(self.base_baseline, "pin", {FAILING: ("not_ok", "42883", "-"),
                                                    ("select2.test", 5): ("not_ok", "22012", "-")})
        r = self.report()
        self.assertTrue(r.passed, r.problems)
        self.assertIn("+4.167 pp (-1 failing)", self.row(r.text, "B0"))

    def test_stale_exceptions(self):
        self.compare["1/2"]["C2"]["stale_exceptions"] = [
            {"file": "evidence/e.test", "line": 5, "reason": "fixed upstream"}]
        r = self.report()
        self.assertFalse(r.passed)
        self.assertIn("prolly: 1 stale exceptions", r.problems)
        self.assertIn("<summary>Stale exceptions (1)</summary>", r.text)
        self.assertIn("- `evidence/e.test:5` (prolly): fixed upstream", r.text)

    def test_missing_shard(self):
        self.skip = {("2/2", "heap"), ("2/2", "prolly")}
        r = self.report()
        self.assertFalse(r.passed)
        self.assertIn("shard 2/2: missing", r.problems)
        self.assertIn("heap: C3 index/a.test: in no shard", r.problems)
        self.assertIn("- shard 2/2: missing", r.text)

    def test_missing_engine_and_incomplete_shard(self):
        self.skip = {("1/2", "prolly")}
        r = self.report()
        self.assertFalse(r.passed)
        self.assertIn("shard 1/2: no prolly result set", r.problems)
        self.skip = set()
        dirs = self.sets()
        results = dirs[-1] / "results.jsonl"
        lines = [json.loads(x) for x in results.read_text().splitlines()]
        lines[0].update(status="error")
        results.write_text("".join(json.dumps(x) + "\n" for x in lines))
        r = creport.build(dirs, 2, self.base_baseline, baseline=self.baseline,
                          pin_file=self.pin, corpus=self.corpus)
        self.assertFalse(r.passed)
        self.assertTrue(any("shard 2/2 prolly" in p for p in r.problems), r.problems)

    def test_engine_row_fails_with_its_own_problems(self):
        self.skip = {("2/2", "prolly")}
        r = self.report()
        self.assertTrue(self.row(r.text, "prolly").endswith("| FAIL |"))
        self.assertTrue(self.row(r.text, "B0").endswith("| baseline: 0 new, 0 fixed |"))

    def test_expected_engine_missing_everywhere(self):
        r = self.report(engines=["prolly", "other"])
        self.assertFalse(r.passed)
        self.assertIn("shard 1/2: no other result set", r.problems)

    def test_no_or_other_base_baseline_is_a_note(self):
        self.base_baseline.unlink()
        r = self.report()
        self.assertTrue(r.passed, r.problems)
        self.assertIn("| 95.833% | n/a |", self.row(r.text, "B0"))
        self.assertIn("_Note: no heap baseline at the base", r.text)
        cbaseline.write(self.base_baseline, "older", {})
        r = self.report()
        self.assertTrue(r.passed, r.problems)
        self.assertIn("| n/a |", self.row(r.text, "prolly"))
        self.assertIn("not pin: no change against the base", r.text)

    def test_incomplete_records_are_not_failing_against_the_base(self):
        for am in ("heap", "prolly"):
            self.records["1/2", am]["C1"][0].update(status="timeout")
        self.compare["1/2"]["C1"]["counts"] = {"agree": 14, "timeout": 1}
        self.compare["1/2"]["C1"]["gate_failures"] = 1
        r = self.report()
        self.assertFalse(r.passed)
        self.assertIn("| ±0 |", self.row(r.text, "B0"))
        self.assertIn("1 incomplete", self.row(r.text, "B0"))

    def test_unreadable_inputs_still_write_a_report(self):
        dirs = self.sets()
        self.baseline.write_text("not gzip")
        (dirs[0] / "config.json").unlink()
        r = creport.build(dirs, 2, self.base_baseline, baseline=self.baseline,
                          pin_file=self.root / "no-pin", corpus=self.corpus)
        self.assertFalse(r.passed)
        self.assertTrue(r.text.startswith(creport.MARKER))
        self.assertTrue(any(p.startswith("no corpus pin") for p in r.problems), r.problems)

    def test_stale_comparison_is_not_trusted(self):
        dirs = self.sets()
        # (edited after cgate stamped it: not to be trusted, and no runner here)
        (dirs[1] / "compare" / "2026-10-18-C-heap" / "C1" / "compare.json").write_text(
            json.dumps({"counts": {"agree": 99}, "gate_failures": 0, "stale_exceptions": []}))
        r = creport.build(dirs, 2, self.base_baseline, baseline=self.baseline,
                          pin_file=self.pin, corpus=self.corpus)
        self.assertFalse(r.passed)
        self.assertTrue(any("no comparison with heap" in p for p in r.problems), r.problems)

    def test_failed_job_fails_the_report(self):
        r = self.report(failed_jobs=["shard 3"])
        self.assertFalse(r.passed)
        self.assertIn("- job failed: shard 3", r.text)

    def test_truncation(self):
        text = creport.MARKER + "\n<details><summary>x</summary>\n" + "a line of text\n" * 10000
        cut = creport.truncate(text, 65000, "https://ci/run/1")
        self.assertLessEqual(len(cut), 65000)
        self.assertTrue(cut.startswith(creport.MARKER))
        self.assertEqual(cut.count("<details>"), cut.count("</details>"))
        self.assertTrue(cut.endswith("**Truncated at 65,000 characters**: the whole report "
                                     "is in the [artifacts](https://ci/run/1).\n"))
        self.assertEqual(creport.truncate("short", 65000), "short")

        self.diffs["2/2"]["C4"] = [{
            "file": "random/r.test", "line": n, "class": "regression",
            "baseline": {"status": "ok"}, "engine": {"status": "not_ok", "msg": "m" * 150}}
            for n in range(1, 400)]
        r = self.report(max_chars=4000)
        self.assertLessEqual(len(r.text), 4000)
        self.assertIn("**Truncated at 4,000 characters**", r.text)
        self.assertEqual(r.text.count("<details>"), r.text.count("</details>"))
        self.assertFalse(r.passed)


if __name__ == "__main__":
    unittest.main()
