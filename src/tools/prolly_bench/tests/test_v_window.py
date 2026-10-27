"""The V suite's latency workloads measure inside the history window: the
window set before any history, and the root path checked after."""
import unittest
from types import SimpleNamespace

from pbench.core.interfaces import Outcome, Window
from pbench.workloads import v_suite
from pbench.workloads.v_suite import V1, V3, V7, V10, PATH_COUNTERS, path_failures


class FakeCluster:
    """Answers the window's statements: path counters that grow by grow
    each time the workload's operations run."""
    def __init__(self, grow=None):
        self.sent, self.counts = [], dict.fromkeys(PATH_COUNTERS, 0)
        self.grow = grow or {}

    def sql(self, q, **kw):
        self.sent.append(q)
        out = ""
        if "pg_stat_prolly" in q:
            out = "\n".join(f"{k}={v}" for k, v in self.counts.items())
        return SimpleNamespace(stdout=out)

    def scalar(self, q, **kw):
        self.sent.append(q)
        return "3"

    def operate(self):
        for k, v in self.grow.items():
            self.counts[k] += v


class NullWindow(Window):
    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


def measured(workload, cluster):
    workload.operations = lambda ctx: (cluster.operate(), Outcome(metrics={"x_ms": 1.0}))[1]
    ctx = SimpleNamespace(cluster=cluster, dbname="vdb", params={"background": "none"},
                          session_options=lambda: {})
    return workload.measure(ctx, NullWindow())


class VWindowTest(unittest.TestCase):
    def test_latency_workloads_set_the_window_and_counter_gates_dont(self):
        self.assertTrue(all(w.in_window for w in (v_suite.V1, v_suite.V2, v_suite.V3,
                                                  v_suite.V4, v_suite.V6)))
        self.assertFalse(any(w.in_window for w in (v_suite.V7, v_suite.V8, v_suite.V9, v_suite.V10)))
        cluster = FakeCluster()
        ctx = SimpleNamespace(cluster=cluster, dbname="vdb", params={"history_window": "10 years"})
        V1().set_window(ctx)
        self.assertIn("ALTER DATABASE vdb SET prolly.history_window = '10 years';", cluster.sent[0])
        self.assertIn("ALTER DATABASE vdb SET prolly.root_interval_commits = '1';", cluster.sent[0])
        cluster = FakeCluster()
        V7().set_window(SimpleNamespace(cluster=cluster, dbname="vdb", params={}))
        V10().set_window(SimpleNamespace(cluster=cluster, dbname="vdb", params={}))
        self.assertEqual(cluster.sent, [])

    def test_the_root_path_is_reported_beside_the_latencies(self):
        cluster = FakeCluster({"moves_root_kept": 2})
        outcome = measured(V1(), cluster)
        self.assertEqual(outcome.metrics["path_moves_root_kept"], 2.0)
        self.assertEqual(outcome.metrics["path_move_tables_written"], 0.0)
        self.assertEqual(outcome.metrics["x_ms"], 1.0)
        self.assertTrue(any("prolly.history_window = 100 years" in n and "roots kept on main: 3" in n
                            for n in outcome.notes))

    def test_leaving_the_root_path_fails_the_run(self):
        for grow, why in [({"moves_root_missing": 1, "move_tables_written": 1}, "without a root"),
                          ({"move_tables_written": 1}, "wrote rows"),
                          ({"restores_lock_busy": 1}, "for a lock"),
                          ({"severed_refusals": 1}, "past the horizon")]:
            with self.assertRaisesRegex(RuntimeError, why):
                measured(V1(), FakeCluster(grow))
        # (a merge writes rows of its own; a move without a root still fails it)
        measured(V3(), FakeCluster({"move_tables_written": 1}))
        with self.assertRaisesRegex(RuntimeError, "without a root"):
            measured(V3(), FakeCluster({"moves_root_missing": 1}))

    def test_counters_are_read_once_the_operations_sessions_have_exited(self):
        # (their counts are flushed as their backends exit: the read waits
        # for every other client session of the database to be gone, in the
        # same script, before it reads)
        cluster = FakeCluster({"moves_root_kept": 1})
        measured(V1(), cluster)
        reads = [q for q in cluster.sent if "pg_stat_prolly" in q]
        self.assertEqual(len(reads), 2)
        for q in reads:
            self.assertLess(q.index("pg_stat_activity"), q.index("FROM pg_stat_prolly"))
            self.assertIn("pg_stat_clear_snapshot()", q)
            self.assertIn("pid <> pg_backend_pid()", q)
            self.assertIn("backend_type = 'client backend'", q)

    def test_path_failures(self):
        self.assertEqual(path_failures({}, False), [])
        self.assertEqual(path_failures({"moves_root_nearest": 1, "move_tables_written": 1}, True), [])
        self.assertEqual(len(path_failures({"moves_root_missing": 1, "move_tables_written": 1}, False)), 2)


if __name__ == "__main__":
    unittest.main()
