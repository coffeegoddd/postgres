"""The harness's conditions on a run: what it accepts as a good run, and
what it keeps of a failed one."""
import signal
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import build_postgres
from pbench.clusters.local import LocalCluster
from pbench.collectors.gc import GcCollector
from pbench.collectors.recovery import RecoveryCollector
from pbench.collectors.vacuum import VacuumCollector
from pbench.core import env as envmod
from pbench.core import process
from pbench.core.runner import Runner
from pbench.core.validation import comparison_identity
from pbench.workloads._common import BackgroundPgbench
from pbench.workloads.v_suite import V1
from pbench.core.interfaces import NotImplementedByEngine


class ConditionsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)

    def stop_background(self, output):
        bg = object.__new__(BackgroundPgbench)
        bg.out_path = self.path / "pgbench.out"
        bg.out_path.write_text(output)
        bg.out = open(bg.out_path, "a")
        bg.proc = SimpleNamespace(send_signal=lambda sig: None, wait=lambda timeout=None: 0,
                                  returncode=-signal.SIGINT, kill=lambda: None)
        bg.__exit__(None, None, None)

    def test_background_pgbench_stopped_without_its_report(self):
        # (pgbench dies on SIGINT mid-run: only its progress lines are left)
        self.stop_background("progress: 1.0 s, 90.0 tps, lat 1.1 ms stddev 0.2, 0 failed\n"
                             "progress: 2.0 s, 95.0 tps, lat 1.0 ms stddev 0.2, 0 failed\n")
        with self.assertRaisesRegex(RuntimeError, "no useful work"):
            self.stop_background("progress: 1.0 s, 0.0 tps, lat 0.0 ms stddev 0.0, 0 failed\n")
        with self.assertRaisesRegex(RuntimeError, "failed transactions: 3"):
            self.stop_background("progress: 1.0 s, 90.0 tps, lat 1.1 ms stddev 0.2, 3 failed\n")

    def test_build_inside_the_tree_is_not_source(self):
        repo = self.path / "repo"
        repo.mkdir()
        subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
        (repo / "a.c").write_text("int a;\n")
        build = repo / "build-bench" / "x"
        build.mkdir(parents=True)
        with patch.object(build_postgres, "REPO", repo):
            before = build_postgres.sources(build)
            (build / "a.o").write_text("object")
            self.assertEqual(build_postgres.sources(build), before)
            (repo / "b.c").write_text("int b;\n")
            self.assertNotEqual(build_postgres.sources(build), before)

    def test_server_builds_with_their_own_clients_compare(self):
        def identity(**clients):
            return comparison_identity({}, {"client_hashes": {
                "psql": "a", "pgbench": "a", "java": "j", **clients}})
        self.assertEqual(identity(), identity(psql="b", pgbench="b"))
        self.assertNotEqual(identity(), identity(java="k"))

    def test_server_waits_are_not_cut_short(self):
        # (recovery after a crash, or a 16GB checkpoint, can take long)
        cluster = LocalCluster(self.path, self.path, 5432)
        self.addCleanup(cluster.sock_dir.rmdir)
        calls = []
        with patch.object(process, "run", side_effect=lambda args, **kw: (
                calls.append((args, kw)), SimpleNamespace(returncode=0, stdout="", stderr=""))[1]):
            cluster.data_dir.mkdir(parents=True)
            cluster.start()
            cluster.stop()
        for args, kw in calls:
            self.assertIn("3600", args)
            self.assertNotIn("timeout", kw)
        self.assertIsNone(process.run.__kwdefaults__["timeout"])

    def test_failed_stop_still_frees_the_cell(self):
        cluster = LocalCluster(self.path, self.path, 5432)
        cluster.data_dir.mkdir(parents=True)
        with patch.object(cluster, "stop", side_effect=RuntimeError("pg_ctl stop failed")):
            with self.assertRaises(RuntimeError):
                cluster.destroy()
        self.assertFalse(cluster.base.exists())
        self.assertFalse(cluster.sock_dir.exists())

    def test_intel_pstate_turbo_is_cpu_boost(self):
        def files(**values):
            paths = {"boost": "/sys/devices/system/cpu/cpufreq/boost",
                     "no_turbo": "/sys/devices/system/cpu/intel_pstate/no_turbo"}
            return lambda path: next((v for k, v in values.items() if paths[k] == path), "")
        for values, boost in (({"boost": "0"}, "0"), ({"no_turbo": "1"}, "0"),
                              ({"no_turbo": "0"}, "1"), ({}, "")):
            with patch.object(envmod, "_read", side_effect=files(**values)):
                self.assertEqual(envmod._cpu_boost(), boost, values)

    def test_v_suite_needs_an_engine_with_history(self):
        for am in ("heap", "prolly_d2"):
            with self.assertRaisesRegex(SystemExit, "only prolly, prolly_d1 have"):
                Runner(suite="V", cfg={}, table_am=am, label="x", repo=self.path,
                       bin_dir=self.path, work_dir=self.path, out_root=self.path,
                       command="run")

    def test_v_suite_probes_each_engine_in_its_own_sql(self):
        # (page-based prolly: dolt_* functions, in a versioned schema)
        for am, setup, commit in (("prolly", "VERSIONED", "dolt_commit"),
                                  ("prolly_d1", None, "prolly_commit")):
            sql = []
            cluster = SimpleNamespace(sql=lambda q, **kw: sql.append(q))
            ctx = SimpleNamespace(table_am=am, cluster=cluster,
                                  session_options=lambda: {})
            V1().check(ctx)
            probe = next(q for q in sql if "CREATE TABLE target" in q)
            self.assertIn(commit, probe, am)
            self.assertEqual(any(setup and setup in q for q in sql), bool(setup), am)
        with self.assertRaises(NotImplementedByEngine):
            V1().check(SimpleNamespace(table_am="heap"))

    def test_recovery_crashes_the_server_last(self):
        # (VACUUM, space and GC measure the state the workload left)
        self.assertGreater(RecoveryCollector.post_order,
                           max(VacuumCollector.post_order, GcCollector.post_order))


if __name__ == "__main__":
    unittest.main()
