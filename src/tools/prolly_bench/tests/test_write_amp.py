"""The write_amp collector, against a scripted server."""
import json
import re
import tempfile
import unittest
from pathlib import Path
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
        self.autovacuum = []        # workers running, read one per query
        self.flushed = None             # flush LSN, if behind the insert LSN
        self.rmgrs = []                 # pg_get_wal_stats rows
        self.slots = set()
        self.wal_level = "replica"
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
        elif "pg_switch_wal" in sql:
            self.flushed = None
        elif "pg_create_physical_replication_slot" in sql:
            self.slots.add(re.search(r"'(\w+)'", sql).group(1))
        elif "pg_drop_replication_slot" in sql:
            self.slots.remove(re.search(r"'(\w+)'", sql).group(1))
        return SimpleNamespace(stdout="", ok=True)

    def scalar(self, sql, dbname="postgres"):
        if sql == "SHOW wal_level":
            return self.wal_level
        if "pg_current_wal_insert_lsn" in sql:
            return f"0/{self.lsn:X}"
        m = re.search(r"pg_wal_lsn_diff\(pg_current_wal_flush_lsn\(\), '0/([0-9A-F]+)'\)", sql)
        if m:
            flushed = self.lsn if self.flushed is None else self.flushed
            return str(flushed - int(m.group(1), 16))
        if "pg_get_wal_stats" in sql:
            assert self.flushed is None, "read WAL past the flushed position"
            return "\n".join("|".join(str(v) for v in r) for r in self.rmgrs)
        m = re.search(r"pg_wal_lsn_diff\('0/([0-9A-F]+)', '0/([0-9A-F]+)'\)", sql)
        if m:
            return str(int(m.group(1), 16) - int(m.group(2), 16))
        if "autovacuum worker" in sql:
            return str(self.autovacuum.pop(0) if self.autovacuum else 0)
        if "num_requested" in sql:
            return str(self.checkpoints)
        if "backend_type = 'checkpointer'" in sql:
            return "0"
        if "FROM pg_stat_io" in sql:
            return "|".join(str(v) for v in self.io)
        if "pg_stat_get_tuples_inserted" in sql:
            t = re.search(r"to_regclass\('(\w+)'\)", sql).group(1)
            return "|".join(str(v) for v in self.tuples[t])
        if "CREATE EXTENSION" in sql:
            return ""
        if "IS NOT NULL" in sql:
            return "t"
        if "avg(pg_column_size" in sql:
            t = re.search(r"FROM (\w+) t", sql).group(1)
            return str(self.row_bytes[t])
        if "pg_stat_prolly" in sql:
            return "\n".join(f"{k}={v}" for k, v in self.prolly.items())
        raise AssertionError(f"unexpected query: {sql}")


def context(server, am="heap", suite="S", **params):
    raw = tempfile.mkdtemp()
    return SimpleNamespace(cluster=server, table_am=am, tables=["t"], dbname="bench",
                           raw_dir=Path(raw),
                           key=CellKey(suite, "S1", "tiny", "", 1, 1),
                           params={"settled_counters": False, **params})


class WriteAmpTest(unittest.TestCase):
    def setUp(self):
        self._wait = write_amp.wait_for_clients
        self.client_waits = []
        write_amp.wait_for_clients = lambda ctx: self.client_waits.append(self.phase)
        self.phase = "setup"

    def tearDown(self):
        write_amp.wait_for_clients = self._wait

    def run_cell(self, server, ctx, work, outcome):
        c = WriteAmpCollector()
        c.check(ctx)
        self.client_waits.clear()
        self.phase = "before"
        c.before_window(ctx)
        self.assertEqual(server.slots,
                         set() if server.wal_level == "minimal" else {"pbench_write_amp"})
        c.start(ctx)
        self.phase = "window"
        work()
        c.stop(ctx)
        self.phase = "after"
        c.after_window(ctx, outcome)
        # A workload may hold sessions open as the window opens (S6, the
        # V suite's background S1): only the flush waits for clients.
        self.assertTrue(self.client_waits and set(self.client_waits) == {"after"})
        m = c.results(ctx, outcome)
        self.assertEqual(server.slots, set())
        return m

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

    def test_wal_split_by_resource_manager(self):
        s = FakeServer()
        ctx = context(s, am="prolly")
        def work():
            s.lsn += 10_000
            s.flushed = s.lsn - 500      # the tail isn't flushed yet
            s.tuples["t"] = [0, 20_000, 0]
            s.rmgrs = [("Heap", 40, 3000, 2000), ("Heap2", 2, 100, 0),
                       ("Btree", 10, 500, 1000), ("BlockMap", 5, 200, 800),
                       ("ProllyChunks", 3, 1500, 0), ("Transaction", 20, 600, 0),
                       ("XLOG", 4, 200, 0)]
        m = self.run_cell(s, ctx, work, Outcome(transactions=10))
        self.assertEqual(m["wa.wal.row_bytes"], 3100)
        self.assertEqual(m["wa.wal.index_bytes"], 500)
        self.assertEqual(m["wa.wal.blockmap_bytes"], 200)
        self.assertEqual(m["wa.wal.chunk_bytes"], 1500)
        self.assertEqual(m["wa.wal.other_bytes"], 800)
        self.assertEqual(m["wa.wal.fpi_bytes"], 3800)
        self.assertEqual(m["wa.wal.records"], 84)
        self.assertEqual(m["wa.wal.unattributed_bytes"], 10_000 - 9_900)
        self.assertEqual(m["wa.wal.fpi_bytes_per_operation"], 380)
        saved = json.loads((ctx.raw_dir / "write-amp-wal.json").read_text())
        self.assertEqual(saved["rmgrs"][0], {"resource_manager": "Heap", "records": 40,
                                             "record_bytes": 3000, "fpi_bytes": 2000})

    def test_wal_read_before_the_final_checkpoint(self):
        s = FakeServer()
        s.wal_level = "minimal"
        def work():
            s.lsn += 10_000
            s.tuples["t"] = [0, 20_000, 0]
            s.rmgrs = [("Heap", 40, 9_900, 0)]
        # the final checkpoint's record and what follows it aren't the window's
        s.on_checkpoint = lambda: setattr(s, "lsn", s.lsn + 120)
        m = self.run_cell(s, context(s), work, Outcome(transactions=10))
        self.assertEqual(m["wa.wal.total_bytes"], 10_000)
        self.assertEqual(m["wa.wal.row_bytes"], 9_900)

    def test_prolly_materializes_before_the_checkpoint(self):
        s = FakeServer()
        ctx = context(s, am="prolly")
        order = []
        s.on_checkpoint = lambda: order.append("checkpoint")

        def fold():
            order.append("fold")
            if order.count("fold") == 1:
                # before the window: the load's trees, not the window's
                s.prolly["chunk_bytes_appended"] += 50_000_000
                s.lsn += 50_000_000
                return
            s.prolly["chunk_bytes_appended"] += 2_000_000
            s.lsn += 2_000_100
        s.on_prolly_checkpoint = fold

        def work():
            s.lsn += 1_000_000
            s.tuples["t"] = [0, 20_000, 0]
        m = self.run_cell(s, ctx, work, Outcome(transactions=100))
        self.assertEqual(order, ["fold", "checkpoint", "fold", "checkpoint"])
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

    def test_peer_instances_are_counted(self):
        # V4: the clone, fetch and push write to a second instance
        origin, peer = FakeServer(), FakeServer()
        ctx = context(origin, am="prolly", suite="V")
        ctx.peers = [peer]
        peer.on_checkpoint = lambda: peer.io.__setitem__(0, peer.io[0] + 8192 * peer.checkpoints)
        def work():
            origin.lsn += 100_000
            origin.tuples["t"] = [0, 10_000, 0]
            origin.prolly["chunk_bytes_appended"] += 30_000
            peer.lsn += 300_000
            peer.prolly["chunk_bytes_appended"] += 80_000
            peer.tuples["t"] = [999, 0, 0]      # the clone's rows: not logical bytes
        m = self.run_cell(origin, ctx, work, Outcome(transactions=3, unit="operation"))
        self.assertEqual(peer.checkpoints, 2)
        self.assertEqual(self.client_waits, ["after", "after"])
        self.assertEqual(m["wa.instances"], 2)
        self.assertEqual(m["wa.wal.total_bytes"], 400_000)
        self.assertEqual(m["wa.pages.write_bytes"], 8192 * 2)
        self.assertEqual(m["wa.tree.chunk_payload_bytes"], 110_000)
        self.assertEqual(m["wa.logical_bytes"], 1_000_000)      # the origin's rows only
        self.assertEqual(m["wa.server_bytes_per_operation"], (400_000 + 8192 * 2) / 3)

    def test_waits_for_autovacuum_at_each_end(self):
        s = FakeServer()
        # two polls see a worker before the window, one after the window,
        # one after the final checkpoint
        s.autovacuum = [1, 1, 0, 1, 0, 1, 0]
        def work():
            s.tuples["t"] = [10, 0, 0]
        self.run_cell(s, context(s), work, Outcome(transactions=1))
        self.assertEqual(s.autovacuum, [])

    def test_each_tuple_counter_checked_on_its_own(self):
        s = FakeServer()
        s.tuples["t"] = [10, 10, 0]
        def work():
            s.tuples["t"] = [5, 20, 0]      # the sum grew; inserts went back
        with self.assertRaisesRegex(RuntimeError, "went backwards"):
            self.run_cell(s, context(s), work, Outcome(transactions=1))

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
        cfg["postgres"] = {"max_replication_slots": "0"}
        with self.assertRaisesRegex(config.ConfigError, "replication slot"):
            config.validate(cfg, "S")
        # (minimal: no slot; the counter runs have no timed checkpoints)
        cfg["postgres"] = {"wal_level": "minimal", "max_replication_slots": "0"}
        config.validate(cfg, "S")
        cfg["run"]["write_amp_materialize"] = "dolt_commit"
        with self.assertRaisesRegex(config.ConfigError, "write_amp_materialize"):
            config.validate(cfg, "S")


class RunnerOrderTest(unittest.TestCase):
    """The runner calls after_window after every stop, and before any
    results and the post-run checks (which read the tables)."""

    def test_after_window_precedes_results_and_verification(self):
        import tempfile
        from pathlib import Path
        from unittest import mock
        from pbench.core import runner as runner_mod
        from pbench.core.interfaces import Workload

        order = []

        class Probe(Collector):
            name = "probe"
            def before_window(self, ctx): order.append("before_window")
            def start(self, ctx): order.append("start")
            def stop(self, ctx): order.append("stop")
            def after_window(self, ctx, outcome): order.append("after_window")
            def results(self, ctx, outcome):
                order.append("results")
                return {}
            def post(self, ctx, outcome):
                order.append("post")
                return {}

        class Work(Workload):
            id, suite, title = "S1", "S", "probe"
            def prepare(self, ctx): pass
            def measure(self, ctx, window):
                ctx.peers.append(peer)          # (V4's clone)
                with window:
                    order.append("work")
                return Outcome(transactions=1)
            def verify(self, ctx): order.append("workload.verify")

        peer = mock.MagicMock()
        peer.destroy.side_effect = lambda: order.append("peer destroyed")
        cluster = mock.MagicMock()
        cluster.log_since.return_value = ""
        cluster.scalar.return_value = "[]"
        target = mock.MagicMock()
        target.setup_sql.return_value = ""
        target.wrong_am.return_value = []
        with tempfile.TemporaryDirectory() as d:
            r = object.__new__(runner_mod.Runner)
            r.out_dir, r.table_am, r.build = Path(d), "heap", ""
            r.cfg = {"sizes": {"tiny": {}}, "tools": {}, "postgres": {},
                     "run": {"warmup_seconds": 0, "measure_seconds": 1, "quality": "smoke",
                             "collectors": ["probe"], "required_collectors": []}}
            r.info = SimpleNamespace(environment={"postgres": {"binary_hashes": {}}})
            r.keep_failed, r.bin_dir = False, Path(d)
            r._make_cluster = lambda instance: cluster
            with mock.patch.object(runner_mod.registry.collectors, "get", lambda name: Probe), \
                    mock.patch.object(runner_mod.targets, "for_am", lambda am: target), \
                    mock.patch.object(runner_mod.env_mod, "build_provenance", lambda b: None), \
                    mock.patch.object(runner_mod.config_mod, "server_settings", lambda c: {}), \
                    mock.patch.object(runner_mod.correctness, "verify",
                                      lambda ctx, when: order.append(f"correctness.{when}")):
                result = r._run_cell(CellKey("S", "S1", "tiny", "", 1, 1), Work(),
                                     {"seed": 1})
        self.assertEqual(result.status.value, "ok", result.message)
        self.assertEqual(order, ["correctness.before", "before_window", "start", "work", "stop",
                                 "after_window", "results", "correctness.after",
                                 "workload.verify", "post", "peer destroyed"])


if __name__ == "__main__":
    unittest.main()
