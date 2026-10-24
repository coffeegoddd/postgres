import argparse
import tempfile
import unittest
from pathlib import Path

from pbench import differential


def fake_install(root: Path, with_postgis: bool) -> Path:
    """A bin directory whose pg_config names a share directory, with or
    without PostGIS's control file."""
    bin_dir, share = root / "bin", root / "share"
    (share / "extension").mkdir(parents=True)
    bin_dir.mkdir()
    if with_postgis:
        (share / "extension" / "postgis.control").write_text("")
    pg_config = bin_dir / "pg_config"
    pg_config.write_text(f"#!/bin/sh\necho {share}\n")
    pg_config.chmod(0o755)
    return bin_dir


class PostgisOptionTests(unittest.TestCase):
    def test_off_needs_nothing(self):
        args = argparse.Namespace(postgis=False, pg_bin=Path("/nonexistent"))
        self.assertEqual(differential.postgis_options(args), [])

    def test_on_passes_the_runner_flag(self):
        with tempfile.TemporaryDirectory() as d:
            args = argparse.Namespace(postgis=True, pg_bin=fake_install(Path(d), True))
            self.assertEqual(differential.postgis_options(args), ["-postgis"])

    def test_on_without_postgis_is_refused_before_running(self):
        with tempfile.TemporaryDirectory() as d:
            args = argparse.Namespace(postgis=True, pg_bin=fake_install(Path(d), False))
            with self.assertRaises(SystemExit) as e:
                differential.postgis_options(args)
            self.assertIn("no PostGIS", str(e.exception))


if __name__ == "__main__":
    unittest.main()
