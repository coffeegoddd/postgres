"""The suite registry, PostGIS builds' provenance and the targets that
install a suite's extensions.  No database or PostGIS build is needed.

Run: cd src/tools/prolly_bench && python3 -m unittest discover -s tests
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import stat
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from pbench import __main__ as cli
from pbench import postgis, table
from pbench.core import config, env, suites, targets
from pbench.core import runner as runner_mod

HERE = Path(__file__).resolve().parents[1]

FULL_VERSION = ('POSTGIS="3.7.0dev 2b3ceb9" [EXTENSION] PGSQL="200" GEOS="3.15.0-CAPI-1.21.0" '
                'PROJ="9.9.0 NETWORK_ENABLED=OFF URL_ENDPOINT=https://cdn.proj.org '
                'USER_WRITABLE_DIRECTORY=/home/a/.local/share/proj DATABASE_PATH=/usr/share/proj/proj.db" '
                '(compiled against PROJ 9.9.0) GDAL="GDAL 3.13.3 "Iowa City", released 2026/08/13" '
                'LIBXML="2.15.4" LIBJSON="0.19" LIBPROTOBUF="1.5.2" WAGYU="0.5.0 (Internal)" TOPOLOGY RASTER')


@contextlib.contextmanager
def registered(suite: suites.Suite):
    """A suite registered for one test only."""
    suites.load()
    suites.register(suite)
    try:
        yield suite
    finally:
        del suites._suites[suite.letter]


class RegistryTest(unittest.TestCase):
    def test_every_suite_registered_in_order(self):
        self.assertEqual(suites.letters(), ["S", "V", "C", "G", "GV"])
        g, gv = suites.get("G"), suites.get("GV")
        self.assertEqual((g.layout, g.history_only), (suites.STORAGE, False))
        self.assertEqual((gv.layout, gv.history_only), (suites.VERSION_CONTROL, True))
        self.assertEqual(g.extensions, ("postgis", "postgis_raster", "postgis_topology"))
        self.assertEqual(gv.extensions, g.extensions)
        self.assertFalse(suites.get("C").durable)
        self.assertEqual(suites.get("S").extensions, ())

    def test_workload_ids_belong_to_one_suite(self):
        self.assertEqual(suites.of_workload("G3").letter, "G")
        self.assertEqual(suites.of_workload("GV3").letter, "GV")
        self.assertEqual(suites.of_workload("S10").letter, "S")
        for bad in ("X1", "G0", "GV", "S01", "", None, 3):
            self.assertFalse(suites.is_workload_id(bad), bad)
        ids = ["S10", "S2", "GV2", "G10", "G9", "V1", "C3"]
        self.assertEqual(sorted(ids, key=suites.workload_order),
                         ["S2", "S10", "V1", "C3", "G9", "G10", "GV2"])

    def test_registration_checks(self):
        with self.assertRaises(ValueError):
            suites.register(suites.Suite("S", "again", suites.STORAGE, "standard.toml"))
        with self.assertRaises(ValueError):
            suites.Suite("s", "lower case", suites.STORAGE, "standard.toml")
        with self.assertRaises(ValueError):
            suites.Suite("Q", "no layout", "pretty", "standard.toml")
        with self.assertRaises(ValueError):
            suites.Suite("Q", "foreign workload", suites.STORAGE, "standard.toml",
                         workload_params={"S1": frozenset()})
        with self.assertRaises(KeyError):
            suites.get("Q")

    def test_unknown_suite_is_a_config_error(self):
        with self.assertRaisesRegex(config.ConfigError, "unknown suite 'Q'"):
            config.load(HERE / "configs" / "smoke.toml", {}, "Q")

    def test_g_suites_load_the_standard_configs_durably(self):
        for letter in ("G", "GV"):
            cfg = config.load(HERE / "configs" / suites.get(letter).default_config, {}, letter)
            self.assertEqual(cfg["postgres"]["fsync"], "on")
            with self.assertRaises(config.ConfigError):
                config.load(HERE / "configs" / "c_smoke.toml", {}, letter)

    def test_a_registration_brings_its_keys(self):
        """A new suite's size keys and workload parameters are accepted
        only once it registers them, and only for its workloads."""
        cfg = {"sizes": {"small": {"zt_rows": 10}}, "workloads": {"ZT1": {"depth": [1]}}}
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "zt.toml"
            path.write_text("[run]\nsizes = ['small']\n[sizes.small]\nzt_rows = 10\n"
                            "[workloads.ZT1]\ndepth = [1]\nrate = 5\n")
            with self.assertRaisesRegex(config.ConfigError, "unknown size parameter"):
                config.load(path, {}, "S")
            zt = suites.Suite("ZT", "test", suites.STORAGE, "standard.toml",
                              size_keys=frozenset({"zt_rows"}),
                              workload_params={"ZT1": frozenset({"depth"})},
                              rate_workloads=frozenset({"ZT1"}))
            with registered(zt):
                loaded = config.load(path, {}, "ZT")
                self.assertEqual(loaded["sizes"], cfg["sizes"])
                path.write_text(path.read_text().replace("depth", "width"))
                with self.assertRaisesRegex(config.ConfigError, "unknown workload or parameters"):
                    config.load(path, {}, "ZT")
            self.assertNotIn("ZT", suites.letters())

    def test_cli_choices_and_list_come_from_the_registry(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(cli.main(["list"]), 0)
        self.assertIn("G suite (PostGIS storage) workloads:", out.getvalue())
        self.assertIn("GV suite (PostGIS version control) workloads:", out.getvalue())
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            cli.main(["run", "Q", "--am", "heap"])

    def test_history_only_suites_refuse_heap(self):
        with tempfile.TemporaryDirectory() as d:
            cfg = config.load(HERE / "configs" / "smoke.toml", {}, "GV")
            with self.assertRaisesRegex(SystemExit, "GV suite measures version control"):
                runner_mod.Runner(suite="GV", cfg=cfg, table_am="heap", label="x", repo=HERE,
                                  bin_dir=Path(d), work_dir=Path(d), out_root=Path(d), command="")

    def test_layouts(self):
        self.assertTrue(table._storage("G"))
        self.assertFalse(table._storage("GV"))
        self.assertEqual(table.tables_first_metric("G"), table.tables_first_metric("S"))
        self.assertEqual(table.tables_first_metric("GV"), table.tables_first_metric("V"))


class TargetTest(unittest.TestCase):
    def test_extensions_in_the_versioned_schema_on_prolly(self):
        sql = targets.for_am("prolly", ("postgis", "postgis_raster")).setup_sql("db")
        self.assertEqual(sql.splitlines(), [
            "CREATE SCHEMA versioned VERSIONED;",
            "ALTER DATABASE db SET search_path = versioned, public;",
            "SET search_path = versioned, public;",
            "CREATE EXTENSION postgis;",
            "CREATE EXTENSION postgis_raster;"])

    def test_extensions_in_public_on_heap(self):
        self.assertEqual(targets.for_am("heap", ("postgis",)).setup_sql("db"),
                         "CREATE EXTENSION postgis;")
        self.assertEqual(targets.for_am("heap").setup_sql("db"), "")
        self.assertNotIn("EXTENSION", targets.for_am("prolly").setup_sql("db"))

    def test_extension_names_are_checked(self):
        with self.assertRaises(ValueError):
            targets.for_am("heap", ("postgis; DROP DATABASE x",))


def fake_install(root: Path, libraries: dict[str, bytes], provenance: dict | None) -> Path:
    """A bin directory whose pg_config names a pkglibdir holding libraries."""
    bin_dir, lib = root / "bin", root / "lib"
    bin_dir.mkdir(parents=True)
    lib.mkdir()
    for name, content in libraries.items():
        (lib / name).write_bytes(content)
    pg_config = bin_dir / "pg_config"
    pg_config.write_text(f"#!/bin/sh\necho '{lib}'\n")
    pg_config.chmod(pg_config.stat().st_mode | stat.S_IEXEC)
    if provenance is not None:
        (root / "pbench-build.json").write_text(json.dumps(provenance))
    return bin_dir


class ProvenanceTest(unittest.TestCase):
    def test_libraries_are_per_build(self):
        """A build without PostGIS checks pg_walinspect only; one with it
        checks every module its manifest lists."""
        with tempfile.TemporaryDirectory() as d:
            plain = fake_install(Path(d) / "plain", {"pg_walinspect.so": b"w"}, None)
            self.assertEqual(set(env.library_hashes(plain)), {"pg_walinspect.so"})
            names = ["pg_walinspect.so", "postgis-3.so", "postgis_raster-3.so"]
            spatial = fake_install(Path(d) / "spatial", {n: n.encode() for n in names},
                                   {"libraries": names})
            self.assertEqual(set(env.library_hashes(spatial)), set(names))

    def test_g_without_postgis_provenance_warns(self):
        base = {"slt_build_source_hash": None, "extensions": {}}
        w = env.warnings(base, "C")
        self.assertEqual(w, [])
        w = env.warnings({**base, "postgres": {"configure": "", "cflags": "-O2"}, "cpu_governors": ["performance"], "cpu_boost": "0",
                          "loadavg_at_start": "0 0 0", "repo": {}, "filesystem": {},
                          "data_root_attributes": ""}, "G")
        self.assertTrue(any("--with-postgis" in x for x in w), w)
        mismatched = {**base, "extensions": {"postgis": {"library_mismatches": ["geos 3.14.1, pinned 3.15.0"]}}}
        w = env.warnings({**mismatched, "postgres": {"configure": "", "cflags": "-O2"}, "cpu_governors": ["performance"],
                          "cpu_boost": "0", "loadavg_at_start": "0 0 0", "repo": {},
                          "filesystem": {}, "data_root_attributes": ""}, "GV")
        self.assertTrue(any("geos 3.14.1" in x for x in w), w)

    def test_cells_recheck_libraries(self):
        """A module replaced after the run was planned fails the next cell."""
        with tempfile.TemporaryDirectory() as d:
            names = ["pg_walinspect.so", "postgis-3.so"]
            bin_dir = fake_install(Path(d), {n: n.encode() for n in names}, {"libraries": names})
            r = object.__new__(runner_mod.Runner)
            r.out_dir, r.table_am, r.build, r.bin_dir, r.keep_failed = Path(d) / "out", "heap", "", bin_dir, False
            r.cfg = {"sizes": {"tiny": {}}, "tools": {}, "postgres": {}, "workloads": {},
                     "run": {"warmup_seconds": 0, "measure_seconds": 1, "collectors": [],
                             "required_collectors": []}}
            r.info = SimpleNamespace(environment={"postgres": {
                "binary_hashes": {}, "build_provenance": env.build_provenance(bin_dir),
                "library_hashes": env.library_hashes(bin_dir)}})
            cluster = mock.MagicMock()
            cluster.log_since.return_value = ""
            r._make_cluster = lambda instance: cluster
            (bin_dir.parent / "lib" / "postgis-3.so").write_bytes(b"replaced")
            result = r._run_cell(runner_mod.CellKey("G", "G1", "tiny", "", 1, 1),
                                 mock.MagicMock(id="G1"), {"seed": 1})
            self.assertEqual(result.status.value, "error")
            self.assertIn("a server library changed", result.message)
            cluster.create.assert_not_called()


class PostgisPinTest(unittest.TestCase):
    def test_shipped_pin(self):
        pin = postgis.read_pin()
        self.assertRegex(pin["commit"], r"^[0-9a-f]{40}$")
        self.assertEqual(set(pin["libraries"]) >= {"geos", "proj", "gdal"}, True)

    def test_bad_pins_refused(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "p.toml"
            for text in ('commit = "2b3ceb9"\n[libraries]\ngeos="1"\nproj="1"\ngdal="1"\n',
                         'commit = "' + "a" * 40 + '"\n[libraries]\ngeos="1"\nproj="1"\n',
                         'commit = "' + "a" * 40 + '"\n[libraries]\ngeos="1"\nproj="1"\ngdal="1"\nx="1"\n',
                         'commit = "' + "a" * 40 + '"\nbranch = "master"\n'):
                path.write_text(text)
                with self.assertRaises(postgis.PinError, msg=text):
                    postgis.read_pin(path)

    def test_library_versions_and_mismatches(self):
        answers = {("geos-config", "--version"): "3.15.0",
                   ("pkg-config", "--modversion", "proj"): "9.9.0",
                   ("gdal-config", "--version"): "3.12.1",
                   ("pkg-config", "--modversion", "json-c"): "0.19"}
        found = postgis.library_versions(lambda args: answers.get(tuple(args), ""))
        self.assertEqual(found["geos"], "3.15.0")
        self.assertIsNone(found["sfcgal"])
        pin = {"libraries": {"geos": "3.15.0", "proj": "9.9.0", "gdal": "3.13.3", "sfcgal": "2.2.0"}}
        self.assertEqual(postgis.library_mismatches(pin, found),
                         ["gdal 3.12.1, pinned 3.13.3", "sfcgal not found, pinned 2.2.0"])

    def test_build_refuses_other_libraries_before_building(self):
        with tempfile.TemporaryDirectory() as d, \
                mock.patch.object(postgis, "library_versions",
                                  lambda: {"geos": "3.14.1", "proj": "9.9.0", "gdal": "3.13.3"}), \
                mock.patch.object(postgis, "_source") as source:
            with self.assertRaisesRegex(postgis.PinError, "geos 3.14.1, pinned 3.15.0"):
                postgis.build(pg_config=Path(d) / "pg_config", build_dir=Path(d) / "b")
            source.assert_not_called()

    def test_full_version_compares_versions_not_settings(self):
        fields = postgis.full_version_fields(FULL_VERSION)
        self.assertEqual(fields["GEOS"], "3.15.0-CAPI-1.21.0")
        self.assertEqual(fields["PROJ"], "9.9.0")
        self.assertEqual(fields["GDAL"], "3.13.3")
        self.assertEqual(fields["POSTGIS"], "3.7.0dev 2b3ceb9")
        other_user = FULL_VERSION.replace("/home/a/", "/home/b/")
        self.assertEqual(postgis.full_version_mismatches(FULL_VERSION, other_user), [])
        geos = FULL_VERSION.replace("3.15.0-CAPI", "3.15.1-CAPI")
        self.assertEqual(postgis.full_version_mismatches(FULL_VERSION, geos),
                         ["GEOS: built with 3.15.0-CAPI-1.21.0, running 3.15.1-CAPI-1.21.0"])

    def test_cells_refuse_postgis_on_other_libraries(self):
        with tempfile.TemporaryDirectory() as d:
            r = object.__new__(runner_mod.Runner)
            r.extensions = ("postgis",)
            r.info = SimpleNamespace(environment={"extensions": {"postgis": {"full_version": FULL_VERSION}}})
            ctx = SimpleNamespace(dbname="db", raw_dir=Path(d))
            cluster = mock.MagicMock()
            cluster.scalar.side_effect = ['{"postgis": "3.7.0dev"}',
                                          FULL_VERSION.replace('PROJ="9.9.0', 'PROJ="9.8.1')]
            with self.assertRaisesRegex(RuntimeError, "PROJ: built with 9.9.0, running 9.8.1"):
                r._check_extensions(cluster, ctx)
            recorded = json.loads((Path(d) / "extensions.json").read_text())
            self.assertEqual(recorded["extension_versions"], {"postgis": "3.7.0dev"})
            cluster.scalar.side_effect = ['{"postgis": "3.7.0dev"}', FULL_VERSION]
            r._check_extensions(cluster, ctx)


if __name__ == "__main__":
    unittest.main()
