"""Live digest regressions; CI supplies PBENCH_TEST_PG_BIN from its build."""
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from pbench.clusters.local import LocalCluster
from pbench.core.correctness import table_digests


class TableDigestTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        binaries = os.environ.get("PBENCH_TEST_PG_BIN")
        if not binaries:
            raise unittest.SkipTest("set PBENCH_TEST_PG_BIN to run live digest tests")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.cluster = LocalCluster(Path(binaries), Path(cls.tmp.name), 65431)
        cls.addClassCleanup(cls.cluster.destroy)
        cls.cluster.create({"listen_addresses": "''", "fsync": "off"})
        cls.cluster.start()
        # A base type with exact float8 output and a lossy JSON cast. JSON
        # unwraps domains, so a domain would not exercise the custom cast.
        cls.cluster.sql("""
            CREATE TYPE digest_value;
            CREATE FUNCTION digest_value_in(cstring) RETURNS digest_value
                AS 'float8in' LANGUAGE internal IMMUTABLE STRICT;
            CREATE FUNCTION digest_value_out(digest_value) RETURNS cstring
                AS 'float8out' LANGUAGE internal IMMUTABLE STRICT;
            CREATE TYPE digest_value (INPUT = digest_value_in,
                OUTPUT = digest_value_out, INTERNALLENGTH = 8,
                PASSEDBYVALUE, ALIGNMENT = double);
            CREATE FUNCTION digest_value_json(digest_value) RETURNS json
                LANGUAGE sql IMMUTABLE STRICT AS $$ SELECT '0'::json $$;
            CREATE CAST (digest_value AS json)
                WITH FUNCTION digest_value_json(digest_value);
        """)

    def setUp(self):
        self.cluster.sql("DROP TABLE IF EXISTS digest_case, saved")
        self.ctx = SimpleNamespace(cluster=self.cluster, dbname="postgres",
                                   tables=["digest_case"], session_options=lambda: {})

    def digest(self):
        return table_digests(self.ctx)["digest_case"]

    def test_lossy_json_cast_does_not_hide_value_changes(self):
        self.cluster.sql("CREATE TABLE digest_case (v digest_value); "
                         "INSERT INTO digest_case VALUES ('1.0000000001')")
        before_json = self.cluster.scalar("SELECT row_to_json(t) FROM digest_case t")
        before = self.digest()
        self.cluster.sql("UPDATE digest_case SET v = '1.0000000002'")
        self.assertEqual(before_json, self.cluster.scalar(
            "SELECT row_to_json(t) FROM digest_case t"))
        self.assertNotEqual(before["rows_sha256"], self.digest()["rows_sha256"])

    def test_multiset_nulls_duplicates_and_framing(self):
        # t::text would select column t instead of the whole aliased row.
        self.cluster.sql(r"""
            CREATE TABLE digest_case (t text, v text);
            INSERT INTO digest_case VALUES
                (NULL, ''), ('', NULL), ('(,)', E'a\nb'),
                (E'\\', '"'), ('duplicate', 'x'), ('duplicate', 'x');
            CREATE TABLE saved AS TABLE digest_case;
        """)
        before = self.digest()
        self.cluster.sql("TRUNCATE digest_case; INSERT INTO digest_case "
                         "SELECT * FROM saved ORDER BY t DESC NULLS FIRST")
        self.assertEqual(before, self.digest())
        self.cluster.sql("DELETE FROM digest_case WHERE ctid = "
                         "(SELECT ctid FROM digest_case WHERE t = 'duplicate' LIMIT 1)")
        self.assertNotEqual(before["rows_sha256"], self.digest()["rows_sha256"])
        before = self.digest()
        self.cluster.sql("UPDATE digest_case SET v = 'changed' WHERE t = 'duplicate'")
        self.assertNotEqual(before["rows_sha256"], self.digest()["rows_sha256"])
        before = self.digest()
        self.cluster.sql("UPDATE digest_case SET v = NULL WHERE t IS NULL")
        self.assertNotEqual(before["rows_sha256"], self.digest()["rows_sha256"])

    def test_output_settings_are_fixed(self):
        self.cluster.sql(r"""
            CREATE TABLE digest_case (f float8, d timestamptz, i interval, b bytea);
            INSERT INTO digest_case VALUES (1.0000000001, '2026-01-02 03:04:05+00',
                '1 year 2 months 3 days 04:05:06', '\x00ff'::bytea);
        """)
        before = self.digest()
        self.ctx.session_options = lambda: {
            "timezone": "America/Los_Angeles", "datestyle": "SQL,DMY",
            "intervalstyle": "sql_standard", "bytea_output": "escape",
            "extra_float_digits": "0"}
        self.assertEqual(before, self.digest())


if __name__ == "__main__":
    unittest.main()
