"""Exercise the actual COPY generator beyond int4 multiplication's limit."""
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from pbench.clusters.local import LocalCluster
from pbench.workloads.s_suite import S4


class S4GenerationTest(unittest.TestCase):
    def test_nightly_scale_generation(self):
        binaries = os.environ.get("PBENCH_TEST_PG_BIN")
        if not binaries:
            self.skipTest("set PBENCH_TEST_PG_BIN to run live workload tests")
        with tempfile.TemporaryDirectory() as tmp:
            cluster = LocalCluster(Path(binaries), Path(tmp), 65430)
            try:
                cluster.create({"listen_addresses": "''", "fsync": "off"})
                cluster.start()
                rows = 500_000
                ctx = SimpleNamespace(cluster=cluster, dbname="postgres",
                                      size_params={"s4_rows": rows},
                                      session_options=lambda: {})
                workload = S4()
                workload.prepare(ctx)
                cluster.sql(f"COPY s4 FROM '{workload._file(ctx)}'")
                self.assertEqual(cluster.scalar(f"""
                    SELECT count(*) = {rows} AND min(id) = 1
                       AND max(id) = {rows} AND count(DISTINCT k) = {rows}
                       AND bool_and(k = id * 7919 % {rows})
                       AND bool_and(payload = md5(id::text) || md5((id + 1)::text))
                    FROM s4"""), "t")
            finally:
                cluster.destroy()


if __name__ == "__main__":
    unittest.main()
