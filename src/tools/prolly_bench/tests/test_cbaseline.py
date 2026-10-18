import gzip
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from pbench import cbaseline
from pbench.table import load_set
from evidence_helpers import c_result

SHARD = {"run": {"shard": "1/1", "workloads": ["C3"]}}


def records(**changes):
    """Four records of one file: two pass, one fails, one is skipped; each
    change replaces a line's record."""
    out = {1: {"status": "ok"}, 5: {"status": "ok"},
           9: {"status": "not_ok", "sqlstate": "42883", "s": "digest-9"},
           13: {"status": "skipped"}}
    for line, r in changes.items():
        out[int(line[1:])] = r
    return [{"file": "index/a.test", "line": n, **r} for n, r in sorted(out.items())]


class BaselineTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.path = self.root / "baseline.tsv.gz"
        cbaseline.write(self.path, "pin", {
            ("index/a.test", 9): ("not_ok", "42883", "digest-9"),
            # (another file: outside the run, so not checked)
            ("index/other.test", 3): ("not_ok", "22012", "-")})
        self.n = 0

    def check(self, recs, commit="pin", config=SHARD):
        self.n += 1
        rs = c_result(self.root / f"run{self.n}", "heap", {"C3": recs}, config, commit)
        return cbaseline.check([rs], cbaseline.read(self.path), "pin")

    def test_unchanged_outcome_passes(self):
        c = self.check(records())
        self.assertTrue(c.ok, c.messages())
        self.assertEqual((len(c.files), c.records), (1, 4))
        self.assertIn("pass", c.summary())

    def test_newly_failing_record_is_a_shared_code_regression(self):
        c = self.check(records(l5={"status": "not_ok", "sqlstate": "22012"}))
        self.assertFalse(c.ok)
        self.assertEqual([(d["line"], d["now"]) for d in c.new], [(5, ["not_ok", "22012", "-"])])
        self.assertIn("regression in shared code: index/a.test:5: heap now fails "
                      "(not_ok 22012 -), not in the baseline", c.messages())

    def test_listed_record_that_passes_must_leave_the_file(self):
        c = self.check(records(l9={"status": "ok"}))
        self.assertFalse(c.ok)
        self.assertEqual([d["line"] for d in c.fixed], [9])
        self.assertIn("fixed: index/a.test:9: heap passes now; remove it from the baseline "
                      "(prolly_bench cbaseline update)", c.messages())

    def test_changed_signature_is_a_regression(self):
        for change, now in (({"sqlstate": "22012"}, ["not_ok", "22012", "digest-9"]),
                            ({"s": "digest-other"}, ["not_ok", "42883", "digest-other"]),
                            ({"status": "crashed"}, ["crashed", "42883", "digest-9"])):
            with self.subTest(change=change):
                c = self.check(records(l9={"status": "not_ok", "sqlstate": "42883",
                                           "s": "digest-9", **change}))
                self.assertFalse(c.ok)
                self.assertEqual([d["now"] for d in c.changed], [now])
                self.assertIn("heap fails differently: baseline not_ok 42883 digest-9, now "
                              + " ".join(now), c.messages()[0])

    def test_timeout_is_incomplete_not_a_regression(self):
        for status in ("timeout", "did_not_run"):
            with self.subTest(status=status):
                c = self.check(records(l5={"status": status}, l9={"status": status}))
                self.assertFalse(c.ok)
                self.assertEqual((c.new, c.changed, c.fixed), ([], [], []))
                self.assertEqual(len(c.incomplete), 2)
                self.assertTrue(c.messages()[0].startswith(
                    "incomplete: 2 records timed out or didn't run (not a regression"))

    def test_other_corpus_or_engine_fails(self):
        c = self.check(records(), commit="other")
        self.assertTrue(any("ran sqllogictest other" in p for p in c.problems))
        rs = c_result(self.root / "prolly", "prolly", {"C3": records()}, SHARD)
        c = cbaseline.check([rs], cbaseline.read(self.path), "pin")
        self.assertTrue(any("not a C suite run on heap" in p for p in c.problems))
        c = cbaseline.check([], cbaseline.read(self.path), "newpin")
        self.assertTrue(any("regenerate it" in p for p in c.problems))

    def test_missing_workload_is_incomplete(self):
        c = self.check(records(), config={"run": {"shard": "1/2", "workloads": ["C3", "C4"]}})
        self.assertIn("incomplete: no records for C4", c.problems[0])
        # (an unsharded run is all four workloads)
        c = self.check(records(), config={"run": {"workloads": ["C3"]}})
        self.assertIn("incomplete: no records for C1", c.problems[0])

    def test_result_set_validation_counts(self):
        rs = c_result(self.root / "sealed", "heap", {"C3": records(l5={"status": "timeout"})}, SHARD)
        c = cbaseline.check([load_set(rs.path)], cbaseline.read(self.path), "pin")
        # (a record that timed out is incomplete, not an invalid result set)
        self.assertEqual(c.problems, [])
        self.assertEqual(len(c.incomplete), 1)
        (rs.path / "config.json").write_text("{}")
        c = cbaseline.check([load_set(rs.path)], cbaseline.read(self.path), "pin")
        self.assertTrue(any("artifact changed: config.json" in p for p in c.problems), c.problems)
        self.assertEqual(c.records, 0)

    def test_changed_or_unknown_corpus_checkout(self):
        rs = c_result(self.root / "dirty", "heap", {"C3": records()}, SHARD)
        for git, text in (({"commit": "pin", "dirty_files": 3}, "local changes (3 files)"),
                          ({"commit": "pin"}, "local changes (unknown files)")):
            (rs.path / "environment.json").write_text(json.dumps({"sqllogictest": git}))
            c = cbaseline.check([rs], cbaseline.read(self.path), "pin")
            self.assertTrue(any(text in p for p in c.problems), c.problems)
        (rs.path / "environment.json").unlink()
        c = cbaseline.check([rs], cbaseline.read(self.path), "pin")
        self.assertTrue(any("ran sqllogictest ?" in p for p in c.problems), c.problems)

    def test_read_refuses_duplicate_or_unsorted_lines(self):
        for lines in (["a.test\t5\tnot_ok\t-\t-", "a.test\t5\tnot_ok\t-\t-"],
                      ["b.test\t1\tnot_ok\t-\t-", "a.test\t1\tnot_ok\t-\t-"]):
            self.path.write_bytes(gzip.compress(
                ("# sqllogictest pin\n" + "\n".join(lines) + "\n").encode()))
            with self.assertRaisesRegex(ValueError, "duplicate or out of order"):
                cbaseline.read(self.path)

    def test_listed_record_missing_from_its_file(self):
        c = self.check([r for r in records() if r["line"] != 9])
        self.assertTrue(any("baseline records aren't in the run's files" in p for p in c.problems))

    def test_update_round_trips_and_refuses_partial_runs(self):
        checkout = self.enterContext(
            patch.object(cbaseline, "_checkout_state", return_value=("pin", 0)))
        corpus = self.root / "corpus" / "test"
        full = {"C1": [f"select{i}.test" for i in range(1, 6)], "C2": ["evidence/in1.test"],
                "C3": ["index/a.test"], "C4": ["random/r.test"]}
        for name in sum(full.values(), []):
            (corpus / name).parent.mkdir(parents=True, exist_ok=True)
            (corpus / name).write_text("x")
        others = {w: [{"file": fs[0], "line": 1, "status": "ok"}] for w, fs in full.items()
                  if w != "C3"}
        rs = c_result(self.root / "full", "heap", {**others, "C3": records()})
        out = self.root / "new.tsv.gz"
        # (C1 has five files; this run has one)
        self.assertTrue(any("not a full run" in p
                            for p in cbaseline.update([rs], out, "pin", corpus)))
        self.assertFalse(out.exists())
        others["C1"] = [{"file": f, "line": 1, "status": "ok"} for f in full["C1"]]
        rs = c_result(self.root / "full2", "heap", {**others, "C3": records()})
        self.assertEqual(cbaseline.update([rs], out, "pin", corpus), [])
        self.assertEqual(sorted(p.name for p in self.root.glob("new*")), ["new.tsv.gz"])
        with gzip.open(out, "rt") as f:
            self.assertEqual(f.read(), "# sqllogictest pin\nindex/a.test\t9\tnot_ok\t42883\tdigest-9\n")
        first = out.read_bytes()
        self.assertEqual(cbaseline.update([rs], out, "pin", corpus), [])
        self.assertEqual(out.read_bytes(), first)
        self.assertTrue(cbaseline.check([rs], cbaseline.read(out), "pin").ok)

        (corpus / "index" / "b.test").write_text("x")
        self.assertTrue(any("not a full run" in p for p in cbaseline.update([rs], out, "pin", corpus)))
        (corpus / "index" / "b.test").unlink()
        slow = c_result(self.root / "slow", "heap",
                        {**others, "C3": records(l5={"status": "timeout"})})
        problems = cbaseline.update([slow], out, "pin", corpus)
        self.assertTrue(problems[0].startswith("incomplete: 1 records"))
        self.assertEqual(out.read_bytes(), first)
        # (the checkout is listed as it is now, so it must still be the pin)
        for state in (("other", 0), ("pin", 2)):
            checkout.return_value = state
            self.assertTrue(any("not the pin" in p for p in cbaseline.update([rs], out, "pin", corpus)))
        checkout.side_effect = ValueError("not a checkout of the corpus")
        self.assertTrue(any("cannot list the corpus" in p
                            for p in cbaseline.update([rs], out, "pin", corpus)))

    def test_checked_in_baseline_matches_the_pin(self):
        self.assertEqual(cbaseline.read().commit, cbaseline.pin())


if __name__ == "__main__":
    unittest.main()
