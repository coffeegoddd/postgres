import json
import gzip
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from pbench import ccompare
from pbench.table import ResultSet
from evidence_helpers import seal


class GateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = self.result("heap")
        self.engine = self.result("prolly")
        self.comparison = {"C1": {"counts": {"agree": 12, "postgres_semantics": 2},
                                  "stale_exceptions": []}}

    def result(self, am):
        path = Path(self.tmp.name) / am
        raw = path / "raw" / "slt"
        raw.mkdir(parents=True)
        records = [{"file": "a.test", "line": i + 1, "version": 2,
                    "status": "ok" if i < 12 else "not_ok", "o": "a", "s": "a"}
                   for i in range(14)]
        with gzip.open(raw / "records.jsonl.gz", "wt") as f:
            f.writelines(json.dumps(r) + "\n" for r in records)
        with gzip.open(raw / "planned.jsonl.gz", "wt") as f:
            f.writelines(json.dumps({"file": r["file"], "line": r["line"], "skip": False}) + "\n"
                         for r in records)
        (raw / "corpus.json").write_text(json.dumps({"a.test": "source-hash"}))
        (raw / "files.jsonl").write_text(json.dumps({"file": "a.test", "tables": 1}) + "\n")
        (raw / "summary.json").write_text(json.dumps({
            "total": 14, "files": 1, "counts": {"ok": 12, "not_ok": 2},
            "wrong_am_files": [], "unmatched_digests": 0, "unparsed_lines": 0, "am": am,
        }))
        results = [{"key": {"workload": "C1"}, "status": "ok", "raw_dir": "raw",
                    "metrics": {"records": 14, "skipped": 0, "ok": 12}}]
        (path / "results.jsonl").write_text(json.dumps(results[0]) + "\n")
        seal(path, "C", am)
        return ResultSet(path, "C", "B0" if am == "heap" else am, am, "", results)

    def gate(self):
        with patch.object(ccompare, "compare", return_value=self.comparison):
            return ccompare.gate(self.base, self.engine, ["C1"])

    def test_complete_matching_run(self):
        self.assertEqual(self.gate(), [])

    def test_findings_cannot_pass(self):
        for kind in ("regression", "hidden_regression", "order_only", "engine_only_pass",
                     "not_implemented", "timeout", "did_not_run", "crashed", "unmatched"):
            with self.subTest(kind=kind):
                self.comparison["C1"]["counts"] = {kind: 1}
                self.assertTrue(self.gate())

    def test_missing_and_duplicate_cells(self):
        self.engine.results.append(dict(self.engine.results[0]))
        self.assertTrue(self.gate())
        self.engine.results.clear()
        self.assertTrue(self.gate())

    def test_missing_or_incomplete_artifacts(self):
        path = self.engine.path / "raw/slt/summary.json"
        summary = json.loads(path.read_text())
        for change in ({"wrong_am_files": ["bad.test"]}, {"total": 0},
                       {"counts": {"timeout": 1}}, {"unmatched_digests": 1}):
            path.write_text(json.dumps({**summary, **change}))
            self.assertTrue(self.gate())
        path.unlink()
        self.assertTrue(self.gate())

    def test_unparsed_missing_duplicate_and_tampered_evidence(self):
        raw = self.engine.path / "raw/slt"
        summary = json.loads((raw / "summary.json").read_text())
        (raw / "summary.json").write_text(json.dumps({**summary, "unparsed_lines": 37}))
        seal(self.engine.path)
        self.assertTrue(self.gate())
        (raw / "summary.json").write_text(json.dumps(summary))
        with gzip.open(raw / "records.jsonl.gz", "at") as f:
            f.write(json.dumps({"file": "a.test", "line": 1, "version": 2, "status": "ok"}) + "\n")
        seal(self.engine.path)
        self.assertTrue(self.gate())

    def test_report_cannot_pass_empty_comparison(self):
        with patch.object(ccompare, "compare", return_value={}):
            self.assertIn("| total | 0 | FAIL |", ccompare.to_markdown([self.base, self.engine]))

    def test_missing_comparison_and_stale_exception(self):
        self.comparison["C1"]["stale_exceptions"] = [{"file": "old.test"}]
        self.assertTrue(self.gate())
        self.comparison.clear()
        self.assertTrue(self.gate())


if __name__ == "__main__":
    unittest.main()
