"""V11, generational retires under churn: the keys a cycle updates, the
timed session's parsing, the store's files on disk, the standby's replay
watch, a retire's records, the point reads' counts, the end's order, and
the summary of the cycles' records."""
import json
import os
import re
import stat
import struct
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace

from pbench.core import config as config_mod
from pbench.workloads import v_generations as g


class ChurnTest(unittest.TestCase):
    def test_each_cycle_its_own_keys(self):
        rows, churn = 1000, 100
        seen = []
        for cycle in range(1, 4):
            first, stride = g.churn_ids(rows, churn, cycle)
            keys = list(range(first, first + stride * churn, stride))
            self.assertEqual(len(keys), churn)
            self.assertLessEqual(max(keys), rows)
            seen.append(set(keys))
        self.assertFalse(seen[0] & seen[1])
        self.assertFalse(seen[1] & seen[2])

    def test_churn_past_the_table_refused(self):
        with self.assertRaises(ValueError):
            g.churn_ids(10, 11, 1)


class TimedTest(unittest.TestCase):
    def test_rows_and_times_per_statement(self):
        out = "1 2 3\nTime: 4.5 ms\nSET\nTime: 0.1 ms\nx\ny\nTime: 12.000 ms (00:00.012)\n"
        cluster = SimpleNamespace(sql=lambda *a, **k: SimpleNamespace(ok=True, stdout=out, stderr=""))
        got = g.timed(cluster, "db", ["a", "b", "c"])
        self.assertEqual(got, [(["1 2 3"], 4.5), (["SET"], 0.1), (["x", "y"], 12.0)])

    def test_missing_timing_refused(self):
        cluster = SimpleNamespace(sql=lambda *a, **k: SimpleNamespace(ok=True, stdout="Time: 1 ms\n", stderr=""))
        with self.assertRaises(RuntimeError):
            g.timed(cluster, "db", ["a", "b"])


def table_file(path, section):
    foot = bytearray(g.FOOTER_SIZE)
    struct.pack_into("<I", foot, g.FOOTER_SECTION_LEN, section)
    foot[-8:] = b"PRLYTBL1"
    path.write_bytes(b"\0" * 100 + bytes(foot))


def fake_store_cluster(data, manifests):
    """A cluster whose manifest reads answer from manifests in turn (the
    last repeated): each {"tables": {name: bytes}, "journal_bytes": n,
    "journal_segments": n}."""
    reads = {"sizes": 0, "files": 0}

    def manifest(kind):
        m = manifests[min(reads[kind], len(manifests) - 1)]
        reads[kind] += 1
        return m

    def scalar(q, **k):
        if "prolly_store_sizes()" in q:
            m = manifest("sizes")
            tb = sum(m["tables"].values())
            return json.dumps({"journal_segments": m.get("journal_segments", 1),
                               "journal_bytes": m.get("journal_bytes", 10),
                               "table_files": len(m["tables"]), "table_file_bytes": tb,
                               "settled_files": len(m["tables"]), "settled_bytes": tb})
        if "test_chunk_table_files()" in q:
            return json.dumps(manifest("files")["tables"])
        if "pg_database" in q:
            return "5"
        if "pg_total_relation_size" in q:
            return "0"
        raise AssertionError(q)

    return SimpleNamespace(data_dir=data, scalar=scalar), reads


class StoreTest(unittest.TestCase):
    def store(self, data, table_file_bytes, manifests=None):
        manifests = manifests or [{"tables": {"prolly_table_00ab": table_file_bytes}}]
        cluster, self.reads = fake_store_cluster(data, manifests)
        return g.V11().store(SimpleNamespace(cluster=cluster, dbname="db"))

    def layout(self, d):
        data = Path(d)
        db = data / "base" / "5"
        db.mkdir(parents=True)
        table_file(db / "prolly_table_00ab", 1234)
        (db / "prolly_journal").write_bytes(b"\0" * 10)
        (db / "prolly_manifest").write_bytes(b"\0" * 7)
        (db / "prolly_index").write_bytes(b"\0" * 3)
        (db / "16384").write_bytes(b"\0" * 8192)
        (data / "pg_wal").mkdir()
        (data / "pg_wal" / "000000010000000000000001").write_bytes(b"\0" * 5000)
        (data / "PG_VERSION").write_bytes(b"19\n")
        return data, db

    def test_section_length_from_footer(self):
        with tempfile.TemporaryDirectory() as d:
            data, _ = self.layout(d)
            got = self.store(data, 148)
            self.assertEqual(got["section_bytes_on_disk"], 1234)
            self.assertEqual(got["table_files_on_disk"], 1)
            self.assertEqual(got["store_bytes"], 158)

    def test_store_files_on_disk_and_data_dir_without_wal(self):
        with tempfile.TemporaryDirectory() as d:
            data, _ = self.layout(d)
            got = self.store(data, 148)
            self.assertEqual(got["store_files_bytes_on_disk"], 148 + 10)
            self.assertEqual(got["store_meta_bytes_on_disk"], 7 + 3)
            self.assertFalse(got["store_files_mismatch"])
            self.assertEqual(got["store_disk_minus_manifest_bytes"], 0)
            # (pg_wal on its own; the data directory without it)
            self.assertEqual(got["pg_wal_bytes"], 5000)
            self.assertEqual(got["data_dir_bytes"], 148 + 10 + 7 + 3 + 8192 + 3)

    def test_file_the_manifest_does_not_name_is_flagged(self):
        with tempfile.TemporaryDirectory() as d:
            data, db = self.layout(d)
            table_file(db / "prolly_table_00cd", 10)
            got = self.store(data, 148)
            self.assertTrue(got["store_files_mismatch"])
            self.assertEqual(got["store_disk_minus_manifest_bytes"], 148)
            self.assertEqual(got["table_files_on_disk"], 2)

    def test_journal_append_between_reads_is_not_flagged(self):
        # (the manifest says 10 journal bytes, the segment on disk has 14:
        # the fold worker appended between the read and the walk)
        with tempfile.TemporaryDirectory() as d:
            data, db = self.layout(d)
            (db / "prolly_journal").write_bytes(b"\0" * 14)
            got = self.store(data, 148)
            self.assertFalse(got["store_files_mismatch"])
            self.assertEqual(got["store_disk_minus_manifest_bytes"], 4)

    def test_table_file_added_between_reads_is_not_flagged(self):
        # (a collection added a file after the first read; the manifest
        # read again after the walk names it)
        with tempfile.TemporaryDirectory() as d:
            data, db = self.layout(d)
            table_file(db / "prolly_table_00cd", 10)
            got = self.store(data, 148, [{"tables": {"prolly_table_00ab": 148}},
                                         {"tables": {"prolly_table_00ab": 148, "prolly_table_00cd": 148}}])
            self.assertFalse(got["store_files_mismatch"])
            self.assertEqual(self.reads["files"], 2)

    def test_table_file_of_another_size_is_flagged(self):
        with tempfile.TemporaryDirectory() as d:
            data, _ = self.layout(d)
            got = self.store(data, 148, [{"tables": {"prolly_table_00ab": 150}, "journal_bytes": 8}])
            self.assertEqual(got["store_disk_minus_manifest_bytes"], 0)
            self.assertTrue(got["store_files_mismatch"])


FAKE_PSQL = """import os, sys, time
for line in sys.stdin:
    if line.strip():
        print(f"{time.time():.6f} 0/{os.getpid():X}", flush=True)
"""


class ReplayTest(unittest.TestCase):
    def test_target_before_a_page_header(self):
        page, seg = 8192, 16 << 20
        self.assertEqual(g.replay_target(5 * page + 24, page, seg), 5 * page)
        self.assertEqual(g.replay_target(3 * seg + 40, page, seg), 3 * seg)
        self.assertEqual(g.replay_target(5 * page + 64, page, seg), 5 * page + 64)
        self.assertEqual(g.lsn_int("1/A0"), (1 << 32) + 0xA0)

    def test_polls_on_one_open_session(self):
        with tempfile.TemporaryDirectory() as d:
            psql = Path(d) / "psql"
            psql.write_text(f"#!{sys.executable}\n" + FAKE_PSQL)
            psql.chmod(psql.stat().st_mode | stat.S_IXUSR)
            sb = SimpleNamespace(bin=lambda p: str(psql), client_env=lambda: dict(os.environ))
            sess = g.StandbySession(sb)
            try:
                polls = [sess.poll() for _ in range(20)]
            finally:
                sess.close()
            # (one process answered every poll)
            self.assertEqual(len({lsn for _, lsn in polls}), 1)
            self.assertNotEqual(polls[0][1], os.getpid())

    def test_watch_reports_first_poll_past_target_and_resolution(self):
        seq = iter([(10.000, 5), (10.001, 5), (10.002, 7), (10.003, 9), (10.004, 9)] + [(11.0, 9)] * 10000)
        w = g.ReplayWatch(lambda: next(seq), interval_s=0)
        w.target = 9                # (known before the first poll here)
        w.start()
        prev, hit = w.wait(9)
        self.assertEqual((prev, hit), (10.002, 10.003))
        got = g.replay_times((prev, hit), 10.0015)
        self.assertAlmostEqual(got["standby_replay_ms"], 1.5)
        self.assertAlmostEqual(got["standby_replay_resolution_ms"], 1.0)


class ReplayLateTargetTest(unittest.TestCase):
    def test_polls_before_the_target_was_known_count(self):
        # (the standby has replayed the target by the first poll; the
        # target is known only after many polls, as after the retire's
        # psql exits)
        clock = iter(10.0 + k * 0.001 for k in range(100000))
        w = g.ReplayWatch(lambda: (next(clock), 9), interval_s=0)
        w.start()
        while len(w.polls) < 50:
            time.sleep(0.001)
        prev, hit = w.wait(9)
        self.assertIsNone(prev)
        self.assertEqual(hit, 10.0)
        # (replayed before the retire's end, at 10.0105: <= 0)
        self.assertLess(g.replay_times((prev, hit), 10.0105)["standby_replay_ms"], 0)

    def test_first_past_takes_the_poll_before(self):
        polls = [(1.0, 3), (1.1, 5), (1.2, 9), (1.3, 9)]
        self.assertEqual(g.ReplayWatch.first_past(polls, 9), (1.1, 1.2))
        self.assertEqual(g.ReplayWatch.first_past(polls, 4), (1.0, 1.1))
        self.assertIsNone(g.ReplayWatch.first_past(polls, 10))


class CycleSectionsTest(unittest.TestCase):
    def test_new_section_bytes_are_the_new_files_sections(self):
        with tempfile.TemporaryDirectory() as d:
            data = Path(d)
            db = data / "base" / "5"
            db.mkdir(parents=True)
            (data / "pg_wal").mkdir()
            table_file(db / "prolly_table_00ab", 1000)
            manifests = [{"tables": {"prolly_table_00ab": 148}}]
            cluster, _ = fake_store_cluster(data, manifests)
            cluster.sql = lambda *a, **k: SimpleNamespace(ok=True, stdout="", stderr="")
            base_scalar = cluster.scalar
            cluster.scalar = lambda q, **k: "abc" if "dolt_hashof" in q else base_scalar(q, **k)

            def timed(cl, db_, stmts):
                if any("test_chunk_gc" in x for x in stmts):
                    # (the default collection writes a file of its own)
                    table_file(db / "prolly_table_00cd", 345)
                    manifests[0] = {"tables": {"prolly_table_00ab": 148, "prolly_table_00cd": 148}}
                    return [([""], 1.0), ([""], 1.0), (["1 2 3 4"], 1.0)]
                return [(["0 0 0"], 1.0)]

            w = g.V11()
            w.retire = False
            w.counters = lambda ctx: {}
            w.point_reads = lambda *a: {}
            ctx = SimpleNamespace(cluster=cluster, dbname="db", params={}, size_params={"v_rows": 100})
            w.store(ctx)
            saved = g.timed
            g.timed = timed
            try:
                rec = w.cycle(ctx, 1, 10, "steady")
            finally:
                g.timed = saved
            self.assertEqual(rec["new_section_bytes"], 345)
            self.assertEqual(rec["section_bytes_on_disk"], 1345)


class RetireStepTest(unittest.TestCase):
    """A retire of generations 1 and 2: A (1,000 bytes) has its survivors
    rewritten to A2 (400), B is kept whole, Y is younger; the young set is
    copied to N."""

    GENS_BEFORE = [
        {"name": "A", "chunks": 10, "settled": True, "epoch_lo": 1, "epoch_hi": 1, "garbage": 6, "xrefs": 3},
        {"name": "B", "chunks": 5, "settled": True, "epoch_lo": 2, "epoch_hi": 2, "garbage": 1, "xrefs": 4},
        {"name": "Y", "chunks": 8, "settled": True, "epoch_lo": 3, "epoch_hi": 3, "garbage": 0, "xrefs": 11},
    ]
    GENS_AFTER = [
        {"name": "A2", "chunks": 4, "settled": True, "epoch_lo": 1, "epoch_hi": 1, "garbage": 0, "xrefs": 2},
        {"name": "B", "chunks": 5, "settled": True, "epoch_lo": 2, "epoch_hi": 2, "garbage": 1, "xrefs": 4},
        {"name": "Y", "chunks": 8, "settled": True, "epoch_lo": 3, "epoch_hi": 3, "garbage": 0, "xrefs": 11},
        {"name": "N", "chunks": 9, "settled": True, "epoch_lo": 4, "epoch_hi": 4, "garbage": 0, "xrefs": 6},
    ]

    def test_retire_records(self):
        state = {"retired": False, "files": 0}

        def scalar(q, **k):
            if "test_chunk_retire_choose" in q:
                return "1 2"
            if "test_chunk_table_files" in q:
                state["files"] += 1
                return json.dumps({"A": 1000, "B": 500, "Y": 300} if not state["retired"]
                                  else {"A2": 400, "B": 500, "Y": 300, "N": 700})
            if "pg_current_wal_insert_lsn" in q:
                return "0/1000"
            raise AssertionError(q)

        def sql(script, **k):
            self.assertIn("test_chunk_retire(1, 2)", script)
            state["retired"] = True
            return SimpleNamespace(ok=True, stderr="", stdout=(
                "0/1000\nTime: 0.1 ms\n5 6 900\nTime: 12.0 ms\n"
                "1700000000.500 0/3000\nTime: 0.1 ms\n"))

        def poll():
            # (the standby's clock: one poll before the retire's end, the
            # next 3 ms after it)
            return (1700000000.503, 0x3000) if state["retired"] else (1700000000.499, 0x1000)

        w = g.V11()
        w.retire = True
        w.standby = SimpleNamespace(poll=poll)
        w.generations = lambda ctx: self.GENS_AFTER
        ctx = SimpleNamespace(cluster=SimpleNamespace(scalar=scalar, sql=sql), dbname="db")
        out = w.retire_step(ctx, self.GENS_BEFORE)
        self.assertEqual(out["retire_ms"], 12.0)
        self.assertEqual(out["retire_wal_bytes"], 0x2000)
        self.assertAlmostEqual(out["standby_replay_ms"], 3.0, places=2)
        self.assertAlmostEqual(out["standby_replay_resolution_ms"], 4.0, places=2)
        self.assertEqual(out["retire_r_bytes"], 1500)
        # (R's own: A's 1,000 gone less A2's 400; the young set's the rest)
        self.assertEqual(out["retire_r_bytes_freed"], 600)
        self.assertEqual(out["retire_young_bytes_freed"], 300)
        # (Y's entries only; A's and B's are R's own)
        self.assertEqual(out["retire_younger_section_entries"], 11)
        self.assertNotIn("retire_inbound_xrefs", out)
        self.assertEqual(out["retire_files_kept_whole"], 1)


class PointReadsTest(unittest.TestCase):
    def test_counts_are_the_reads_alone(self):
        events = []
        counts = iter([100, 103, 103 + 3 + 400])

        def sql(script, **k):
            if "prolly_mount(" in script:
                events.append("mount")
                return SimpleNamespace(ok=True, stdout="", stderr="")
            if "prolly_unmount(" in script:
                events.append("unmount")
                return SimpleNamespace(ok=True, stdout="", stderr="")
            events.append("reads")
            keys = re.findall(r"WHERE id = (\d+)", script)
            return SimpleNamespace(ok=True, stderr="",
                                   stdout="".join(f"{k}\nTime: 0.05 ms\n" for k in keys))

        def counters(ctx):
            events.append("counters")
            return {"chunk_file_reads": next(counts)}

        w = g.V11()
        w.counters = counters
        ctx = SimpleNamespace(cluster=SimpleNamespace(sql=sql), dbname="db", size_params={"v_rows": 1000})
        got = w.point_reads(ctx, "abc", 200, 1)
        # (the mount's and the unmount's sessions are outside the counts)
        self.assertEqual(events, ["mount", "counters", "counters", "reads", "counters", "unmount"])
        # (one counter session's own 3 taken off)
        self.assertEqual(got["read_chunk_file_reads_per_read"], 2.0)


class FinishTest(unittest.TestCase):
    def test_conjoin_on_the_cycles_store_then_full_collection(self):
        order = []
        w = g.V11()
        w.retire = False
        w.store = lambda ctx: {"store_bytes": 100, "table_files": 3}
        w.point_reads = lambda ctx, commit, reads, seed: order.append("reads") or {
            "read_ms_p50": 0.1, "read_chunk_file_reads_per_read": 2.0}

        def timed(cluster, db, stmts):
            order.extend("conjoin" if "conjoin" in s else "full" if "prolly_gc(true)" in s else "set"
                         for s in stmts)
            return [(["1 2 3"], 1.0)] * len(stmts) if len(stmts) > 1 else [([""], 1.0)]

        saved = g.timed
        g.timed = timed
        try:
            ctx = SimpleNamespace(cluster=None, dbname="db", params={"conjoin_files": 4})
            w.finish(ctx, [{"read_commit": "abc"}])
        finally:
            g.timed = saved
        self.assertEqual(order, ["reads", "conjoin", "reads", "set", "full"])


class WarmTest(unittest.TestCase):
    def test_ten_severs_before_the_measured_cycles(self):
        self.assertEqual(g.warm_cycles({"window_cycles": 3}), 12)
        self.assertEqual(g.warm_cycles({"window_cycles": 5, "warm_severs": 2}), 6)
        self.assertEqual(g.warm_cycles({"warm_cycles": 10}), 10)
        path = Path(__file__).resolve().parents[1] / "configs" / "v_generations.toml"
        p = config_mod.load(path, {}, "V")["workloads"]["V11"]
        # (the first sever that cuts history is at cycle window_cycles)
        self.assertGreaterEqual(g.warm_cycles(p) - p["window_cycles"] + 1, 10)


class SummaryTest(unittest.TestCase):
    def rec(self, cycle, phase, churn, retire_ms, store):
        return {"cycle": cycle, "phase": phase, "churn": churn, "retired": True,
                "retire_ms": retire_ms, "retire_r_bytes": 10 * churn, "retire_bytes_freed": churn,
                "retire_wal_bytes": 5, "retire_files": 1, "default_gc_ms": 2.0, "commit_ms": 3.0,
                "update_ms": 1.0, "sever_ms": 1.0, "default_gc_table_file_bytes_written": 100,
                "new_file_xrefs": 7, "store_bytes": store, "data_dir_bytes": store * 2,
                "store_files_bytes_on_disk": store, "store_files_mismatch": cycle == 3,
                "pg_wal_bytes": 1000 * cycle, "retire_r_bytes_freed": 0,
                "cycle_table_file_bytes_written": 2**20, "new_section_bytes": 2**20 // 16,
                "section_bytes_on_disk": 70 * cycle, "table_files": cycle,
                "read_ms_p50": 0.1, "read_ms_p95": 0.2, "read_chunk_file_reads_per_read": 2.0}

    def test_steady_and_sweep(self):
        recs = [self.rec(1, "warm", 100, 99.0, 1000)]
        recs += [self.rec(i, "steady", 100, 10.0 + i % 2, 2000) for i in range(2, 6)]
        recs += [self.rec(6, "sweep200", 200, 20.0, 2000)]
        end = {"full_gc_ms": 50.0, "full_gc_bytes_freed": 200, "before_full": {"store_bytes": 2000}}
        m = g.V11.summary(recs, end)
        self.assertEqual(m["steady_cycles"], 4)
        self.assertEqual(m["steady_retire_ms_median"], 10.5)
        self.assertEqual(m["churn100_retire_ms_median"], 10.5)
        self.assertEqual(m["churn200_retire_ms_median"], 20.0)
        self.assertEqual(m["store_bytes_slope_per_cycle"], 0.0)
        self.assertEqual(m["residual_garbage_ratio"], 0.1)
        self.assertEqual(m["retired_bytes_freed_total"], 100 * 5 + 200)
        # (retire_ms against R's MiB: 10 or 11 ms at 1000 bytes, 20 ms at 2000)
        self.assertGreater(m["retire_ms_per_mib_slope"], 0)
        self.assertEqual(m["retires_kept_whole"], 0)
        # (the data directory's slope without pg_wal, which grows)
        self.assertEqual(m["data_dir_bytes_slope_per_cycle"], 0.0)
        self.assertEqual(m["pg_wal_bytes_slope_per_cycle"], 1000.0)
        self.assertEqual(m["store_files_bytes_on_disk_slope_per_cycle"], 0.0)
        self.assertEqual(m["store_files_mismatch_cycles"], 1)
        self.assertEqual(m["retired_r_bytes_freed_total"], 0)
        # (measurement 5a: the cycle's new sections against all it wrote)
        self.assertEqual(m["steady_section_bytes_per_cycle_median"], 2**20 // 16)
        self.assertEqual(m["section_bytes_per_gb_written"], 2**30 / 16)


class ConfigTest(unittest.TestCase):
    def test_config_loads(self):
        path = Path(__file__).resolve().parents[1] / "configs" / "v_generations.toml"
        cfg = config_mod.load(path, {}, "V")
        self.assertEqual(cfg["run"]["workloads"], ["V11"])
        self.assertEqual(cfg["sizes"]["gen10"]["v_rows"], 9615384)

    def test_noretire_config_runs_the_same_cycles(self):
        root = Path(__file__).resolve().parents[1] / "configs"
        cand = config_mod.load(root / "v_generations.toml", {}, "V")
        nr = config_mod.load(root / "v_generations_noretire.toml", {}, "V")
        a, b = cand["workloads"]["V11"], nr["workloads"]["V11"]
        self.assertIs(b["retire"], False)
        self.assertIs(b["standby"], False)
        for k in ("churn", "cycles", "window_cycles", "reads", "full"):
            self.assertEqual(a[k], b[k], k)
        self.assertEqual(g.warm_cycles(a), g.warm_cycles(b))
        self.assertEqual(cand["sizes"], nr["sizes"])
        self.assertEqual(cand["postgres"], nr["postgres"])


if __name__ == "__main__":
    unittest.main()
