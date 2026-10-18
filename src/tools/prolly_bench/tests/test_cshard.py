import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from pbench import cshard
from pbench.core import config as config_mod
from pbench.workloads.c_suite import C3

HERE = Path(__file__).resolve().parents[1]


class ShardTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.corpus = Path(self.tmp.name) / "test"
        sizes = iter([97, 13, 400, 2, 55, 55, 1000, 7, 61, 300, 8, 120, 33, 5, 250])
        names = [f"select{i}.test" for i in range(1, 6)]
        names += ["evidence/in1.test", "evidence/in2.test"]
        names += [f"index/between/{d}/slt_good_{i}.test" for d in (1, 10) for i in range(3)]
        names += [f"random/aggregates/slt_good_{i}.test" for i in range(4)]
        for name, size in zip(names, sizes):
            path = self.corpus / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("x" * size)
        self.cfg = {"workloads": {}, "run": {"workloads": []}}

    def full(self):
        return cshard.workload_files(self.corpus, self.cfg)

    def test_shards_cover_the_corpus_once(self):
        full = self.full()
        self.assertEqual(sum(len(fs) for fs in full.values()), 15)
        for count in range(1, 9):
            with self.subTest(count=count):
                shards = cshard.plan(self.corpus, self.cfg, count)
                self.assertEqual(len(shards), count)
                self.assertEqual(cshard.overlap_and_gaps([s.files for s in shards], full), [])
                every = [f for s in shards for fs in s.files.values() for f in fs]
                self.assertEqual(len(every), len(set(every)))
                self.assertEqual(sorted(every), sorted(f for fs in full.values() for f in fs))

    def test_sampled_config_covers_its_sample(self):
        self.cfg["workloads"] = {"C3": {"sample": "per_directory"}}
        full = self.full()
        self.assertEqual(full["C3"], ["index/between/1/slt_good_0.test",
                                      "index/between/10/slt_good_0.test"])
        shards = cshard.plan(self.corpus, self.cfg, 3)
        self.assertEqual(cshard.overlap_and_gaps([s.files for s in shards], full), [])

    def test_missing_corpus_part_fails_the_plan(self):
        (self.corpus / "select3.test").unlink()
        with self.assertRaisesRegex(ValueError, "not in the corpus: .*select3.test"):
            cshard.plan(self.corpus, self.cfg, 2)

    def test_first_shard_holds_c1_and_c2(self):
        shards = cshard.plan(self.corpus, self.cfg, 4)
        self.assertEqual(len(shards[0].files["C1"]), 5)
        self.assertEqual(len(shards[0].files["C2"]), 2)
        for s in shards[1:]:
            self.assertEqual(s.files["C1"] + s.files["C2"], [])
            self.assertNotIn("C1", s.workloads)

    def test_largest_first_into_the_least_loaded(self):
        sizes = {"a": 10, "b": 9, "c": 6, "d": 4}
        shards = cshard.assign({"C3": list(sizes)}, sizes, 2)
        self.assertEqual([s.files["C3"] for s in shards], [["a", "d"], ["b", "c"]])
        self.assertEqual([s.bytes for s in shards], [14, 15])

    def test_deterministic(self):
        first = [s.files for s in cshard.plan(self.corpus, self.cfg, 3)]
        again = [s.files for s in cshard.plan(self.corpus, self.cfg, 3)]
        self.assertEqual(first, again)

    def test_gaps_overlaps_and_strangers_are_reported(self):
        full = self.full()
        shards = [s.files for s in cshard.plan(self.corpus, self.cfg, 2)]
        dropped = {w: fs[1:] if w == "C4" else fs for w, fs in shards[1].items()}
        self.assertTrue(any("in no shard" in p
                            for p in cshard.overlap_and_gaps([shards[0], dropped], full)))
        twice = {w: fs + shards[0]["C1"] if w == "C1" else fs for w, fs in shards[1].items()}
        self.assertTrue(any("in two shards" in p
                            for p in cshard.overlap_and_gaps([shards[0], twice], full)))
        stranger = {**shards[1], "C4": shards[1]["C4"] + ["random/x.test"]}
        self.assertTrue(any("not in the corpus" in p
                            for p in cshard.overlap_and_gaps([shards[0], stranger], full)))

    def test_apply_records_the_shard_and_its_files(self):
        cfg = {"run": {"workloads": [], "shard": ""}, "workloads": {"C3": {"workers": 2}}}
        shard = cshard.apply(cfg, self.corpus, "2/3")
        self.assertEqual(cfg["run"]["shard"], "2/3")
        self.assertEqual(cfg["run"]["workloads"], shard.workloads)
        self.assertNotIn("C1", shard.workloads)
        spec, files = cshard.recorded(cfg)
        self.assertEqual(spec, "2/3")
        self.assertEqual(files, {w: fs for w, fs in shard.files.items() if fs})
        self.assertEqual(cfg["workloads"]["C3"]["workers"], 2)
        self.assertEqual(cshard.recorded({"run": {"shard": ""}}), ("", {}))

    def test_workload_runs_its_shard_files(self):
        ctx = SimpleNamespace(tools={"sqllogictest": str(self.corpus.parent)},
                              params={"shard_files": ["index/between/1/slt_good_2.test"]})
        self.assertEqual(C3().files(ctx), [str(self.corpus / "index/between/1/slt_good_2.test")])
        ctx.params = {}
        self.assertEqual(C3().files(ctx), [str(self.corpus / "index")])

    def test_unknown_workload_and_empty_shard_are_refused(self):
        for name in ("C5", "S1"):
            with self.subTest(name=name):
                with self.assertRaisesRegex(ValueError, "unknown C workloads"):
                    cshard.plan(self.corpus, self.cfg, 2, [name])
        # (8 files outside C1 and C2: shard 1 plus 8 others at most)
        self.assertEqual(len(cshard.plan(self.corpus, self.cfg, 9)), 9)
        with self.assertRaisesRegex(ValueError, "shard 10/10 would have none"):
            cshard.plan(self.corpus, self.cfg, 10)

    def test_unsharded_identity_is_unchanged(self):
        from pbench.core.validation import comparison_identity
        before = {"run": {"workloads": []}, "postgres": {}}
        self.assertEqual(comparison_identity({**before, "run": {"workloads": [], "shard": ""}}, {}),
                         comparison_identity(before, {}))
        self.assertNotEqual(comparison_identity({**before, "run": {"workloads": [], "shard": "1/2"}}, {}),
                            comparison_identity(before, {}))

    def test_cgate_defaults(self):
        from pbench import ccompare
        path = Path(self.tmp.name) / "rs"
        path.mkdir()
        rs = SimpleNamespace(path=path)
        (path / "config.json").write_text('{"run": {"workloads": ["C1"]}}')
        self.assertEqual(ccompare.run_workloads(rs), ["C1", "C2", "C3", "C4"])
        (path / "config.json").write_text('{"run": {"workloads": ["C3"], "shard": "2/4"}}')
        self.assertEqual(ccompare.run_workloads(rs), ["C3"])

    def test_invalid_shards(self):
        for spec in ("0/4", "5/4", "1", "a/b", "1/0", ""):
            with self.subTest(spec=spec):
                with self.assertRaises(ValueError):
                    cshard.parse(spec)
        cfg = HERE / "configs" / "c_ci.toml"
        with self.assertRaises(config_mod.ConfigError):
            config_mod.load(cfg, {"run": {"shard": "5/4"}}, "C")
        self.assertEqual(config_mod.load(cfg, {"run": {"shard": "2/4"}}, "C")["run"]["shard"], "2/4")
        with self.assertRaises(config_mod.ConfigError):
            config_mod.load(HERE / "configs" / "smoke.toml", {"run": {"shard": "1/2"}}, "S")


if __name__ == "__main__":
    unittest.main()
