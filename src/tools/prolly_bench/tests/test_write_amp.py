"""The write_amp collector, against a scripted server."""
import re
import unittest
from types import SimpleNamespace

from pbench.collectors import write_amp
from pbench.collectors.write_amp import WriteAmpCollector
from pbench.core import config
from pbench.core.interfaces import CellKey, Collector, Outcome
from pbench.core.runner import CollectorWindow


class FakeServer:
    """Answers the collector's queries from counters the test moves."""

    def __init__(self, prolly=False):
        self.lsn = 0
        self.io = [0, 0, 0]
        self.tuples = {"t": [0, 0, 0]}
        self.row_bytes = {"t": 100.0}
        self.prolly = {"chunk_bytes_appended": 0, "nodes_written": 0}
        self.checkpoints = 0
        self.log = []
        self.on_checkpoint = lambda: None
        self.on_prolly_checkpoint = lambda: None

    def sql(self, sql, dbname="postgres", **kw):
        self.log.append(sql.strip())
        if sql.strip() == "CHECKPOINT":
            self.checkpoints += 1
            self.on_checkpoint()
        elif "prolly_checkpoint()" in sql:
            self.on_prolly_checkpoint()
        return SimpleNamespace(stdout="", ok=True)

    def scalar(self, sql, dbname="postgres"):
        if "pg_current_wal_insert_lsn" in sql:
            return f"0/{self.lsn:X}"
        m = re.search(r"pg_wal_lsn_diff\('0/([0-9A-F]+)', '0/([0-9A-F]+)'\)", sql)
        if m:
            return str(int(m.group(1), 16) - int(m.group(2), 16))
        if "num_requested" in sql:
            return str(self.checkpoints)
        if "backend_type = 'checkpointer'" in sql:
            return "0"
        if "FROM pg_stat_io" in sql:
            return "|".join(str(v) for v in self.io)
        if "pg_stat_get_tuples_inserted" in sql:
            t = re.search(r"to_regclass\('(\w+)'\)", sql).group(1)
            return "|".join(str(v) for v in self.tuples[t])
        if "IS NOT NULL" in sql:
            return "t"
        if "avg(pg_column_size" in sql:
            t = re.search(r"FROM (\w+) t", sql).group(1)
            return str(self.row_bytes[t])
        if "pg_stat_prolly" in sql:
            return "\n".join(f"{k}={v}" for k, v in self.prolly.items())
        raise AssertionError(f"unexpected query: {sql}")


def context(server, am="heap", suite="S", **params):
    return SimpleNamespace(cluster=server, table_am=am, tables=["t"], dbname="bench",
                           key=CellKey(suite, "S1", "tiny", "", 1, 1),
                           params={"settled_counters": False, **params})


class WriteAmpTest(unittest.TestCase):
    def setUp(self):
        self._wait = write_amp.wait_for_clients
        write_amp.wait_for_clients = lambda ctx: None

    def tearDown(self):
        write_amp.wait_for_clients = self._wait

    def run_cell(self, server, ctx, work, outcome):
        c = WriteAmpCollector()
        c.before_window(ctx)
        c.start(ctx)
        work()
        c.stop(ctx)
        c.after_window(ctx, outcome)
        return c.results(ctx, outcome)

    def test_heap_counts_the_flush_checkpoint(self):
        s = FakeServer()
        ctx = context(s)
        # The warm-up's dirty pages are written by the first checkpoint,
        # outside the interval; the window's by the second, inside it.
        def flush():
            s.io[0] += 8192 * (100 if s.checkpoints == 1 else 300)
        s.on_checkpoint = flush

        def work():
            s.lsn += 4 << 20
            s.tuples["t"] = [10_000, 20_000, 5]
        m = self.run_cell(s, ctx, work, Outcome(transactions=1000))
        self.assertEqual(s.checkpoints, 2)
        self.assertEqual(m["wa.logical_rows_written"], 30_000)
        self.assertEqual(m["wa.logical_rows_deleted"], 5)
        self.assertEqual(m["wa.logical_bytes"], 3_000_000)
        self.assertEqual(m["wa.pages.write_bytes"], 8192 * 300)
        self.assertEqual(m["wa.wal.total_bytes"], 4 << 20)
        self.assertAlmostEqual(m["wa_wal"], (4 << 20) / 3_000_000)
        self.assertAlmostEqual(m["wa_server"], ((4 << 20) + 8192 * 300) / 3_000_000)
        self.assertEqual(m["wa.server_bytes_per_operation"], ((4 << 20) + 8192 * 300) / 1000)
        self.assertNotIn("wa_tree_payload", m)
        self.assertFalse(any("prolly_checkpoint" in q for q in s.log))

    def test_prolly_materializes_before_the_checkpoint(self):
        s = FakeServer()
        ctx = context(s, am="prolly")
        order = []
        s.on_checkpoint = lambda: order.append("checkpoint")

        def fold():
            order.append("fold")
            s.prolly["chunk_bytes_appended"] += 2_000_000
            s.lsn += 2_000_100
        s.on_prolly_checkpoint = fold

        def work():
            s.lsn += 1_000_000
            s.tuples["t"] = [0, 20_000, 0]
        m = self.run_cell(s, ctx, work, Outcome(transactions=100))
        self.assertEqual(order, ["checkpoint", "fold", "checkpoint"])
        self.assertEqual(m["wa.tree.chunk_payload_bytes"], 2_000_000)
        self.assertEqual(m["wa.tree.materialize_chunk_payload_bytes"], 2_000_000)
        self.assertEqual(m["wa.wal.materialize_bytes"], 2_000_100)
        self.assertEqual(m["wa.wal.total_bytes"], 3_000_100)
        self.assertAlmostEqual(m["wa_tree_payload"], 1.0)

    def test_materialize_mode(self):
        s = FakeServer()
        self.assertEqual(write_amp.materialize_mode(context(s, am="prolly")), "prolly_checkpoint")
        self.assertEqual(write_amp.materialize_mode(context(s, am="prolly", suite="V")), "none")
        self.assertEqual(write_amp.materialize_mode(context(s)), "none")
        self.assertEqual(write_amp.materialize_mode(
            context(s, am="prolly", write_amp_materialize="none")), "none")
        self.assertEqual(write_amp.materialize_mode(
            context(s, write_amp_materialize="prolly_checkpoint")), "none")

    def test_no_ratio_without_logical_writes(self):
        s = FakeServer()
        def work():
            s.lsn += 4096
            s.tuples["t"] = [1, 0, 0]
        m = self.run_cell(s, context(s), work, Outcome(transactions=10))
        self.assertEqual(m["wa.logical_bytes"], 100)
        self.assertIsNone(m["wa_wal"])
        self.assertEqual(m["wa.wal.total_bytes"], 4096)
        s = FakeServer()
        def bulk():
            s.tuples["t"] = [1_000_000, 0, 0]
        m = self.run_cell(s, context(s), bulk, Outcome(transactions=10, read_only=True))
        self.assertIsNone(m["wa_server"])

    def test_counters_going_backwards_fail(self):
        s = FakeServer()
        s.tuples["t"] = [10, 0, 0]
        def work():
            s.tuples["t"] = [5, 0, 0]
        with self.assertRaisesRegex(RuntimeError, "went backwards"):
            self.run_cell(s, context(s), work, Outcome(transactions=1))

    def test_window_runs_before_window_ahead_of_every_start(self):
        order = []

        class Probe(Collector):
            def __init__(self, name):
                self.name = name
            def before_window(self, ctx):
                order.append(("before", self.name))
            def start(self, ctx):
                order.append(("start", self.name))

        status = {}
        window = CollectorWindow(None, [Probe("a"), Probe("b")], status)
        with window:
            pass
        self.assertEqual(order, [("before", "a"), ("before", "b"),
                                 ("start", "a"), ("start", "b")])
        self.assertEqual(len(window.timings["a"]), 2)

    def test_config_refuses_recovery_with_write_amp(self):
        cfg = {"run": config.merge(config.RUN_DEFAULTS, {
            "collectors": ["wal", "write_amp", "recovery"]}),
            "sizes": {}, "workloads": {}}
        with self.assertRaisesRegex(config.ConfigError, "recovery"):
            config.validate(cfg, "S")
        cfg["run"]["collectors"] = ["wal", "write_amp"]
        config.validate(cfg, "S")
        cfg["run"]["write_amp_materialize"] = "dolt_commit"
        with self.assertRaisesRegex(config.ConfigError, "write_amp_materialize"):
            config.validate(cfg, "S")


if __name__ == "__main__":
    unittest.main()
