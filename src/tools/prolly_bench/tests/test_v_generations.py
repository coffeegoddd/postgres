"""V11, generational retires under churn: the keys a cycle updates, the
timed session's parsing, the footer's section length, and the summary of
the cycles' records."""
import struct
import tempfile
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


class SectionTest(unittest.TestCase):
    def test_section_length_from_footer(self):
        with tempfile.TemporaryDirectory() as d:
            data = Path(d)
            (data / "base" / "5").mkdir(parents=True)
            foot = bytearray(g.FOOTER_SIZE)
            struct.pack_into("<I", foot, g.FOOTER_SECTION_LEN, 1234)
            foot[-8:] = b"PRLYTBL1"
            (data / "base" / "5" / "prolly_table_00ab").write_bytes(b"\0" * 100 + bytes(foot))
            (data / "base" / "5" / "prolly_journal_1").write_bytes(b"\0" * 10)
            sizes = ('{"journal_segments": 1, "journal_bytes": 10, "table_files": 1, '
                     '"table_file_bytes": 148, "settled_files": 1, "settled_bytes": 148}')
            answers = iter([sizes, "5", "0"])
            cluster = SimpleNamespace(data_dir=data, scalar=lambda q, **k: next(answers))
            got = g.V11().store(SimpleNamespace(cluster=cluster, dbname="db"))
            self.assertEqual(got["section_bytes_on_disk"], 1234)
            self.assertEqual(got["table_files_on_disk"], 1)
            self.assertEqual(got["store_bytes"], 158)


class SummaryTest(unittest.TestCase):
    def rec(self, cycle, phase, churn, retire_ms, store):
        return {"cycle": cycle, "phase": phase, "churn": churn, "retired": True,
                "retire_ms": retire_ms, "retire_r_bytes": 10 * churn, "retire_bytes_freed": churn,
                "retire_wal_bytes": 5, "retire_files": 1, "default_gc_ms": 2.0, "commit_ms": 3.0,
                "update_ms": 1.0, "sever_ms": 1.0, "default_gc_table_file_bytes_written": 100,
                "new_file_xrefs": 7, "store_bytes": store, "data_dir_bytes": store * 2,
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


class ConfigTest(unittest.TestCase):
    def test_config_loads(self):
        path = Path(__file__).resolve().parents[1] / "configs" / "v_generations.toml"
        cfg = config_mod.load(path, {}, "V")
        self.assertEqual(cfg["run"]["workloads"], ["V11"])
        self.assertEqual(cfg["sizes"]["gen10"]["v_rows"], 9615384)


if __name__ == "__main__":
    unittest.main()
