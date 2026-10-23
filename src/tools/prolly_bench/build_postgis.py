"""Build the pinned PostGIS (configs/postgis.toml) against an existing
server installation, with provenance in BUILD_DIR/pbench-postgis.json.

For a development install (the requirement tests' check-postgis installs
from BUILD_DIR/src into their temporary installation).  Benchmark builds
use build_postgres.py --with-postgis, which builds the server and PostGIS
together and records both in one manifest.
"""
import argparse
import json
from pathlib import Path

from pbench import postgis


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--pg-config", type=Path, required=True, help="the installation's pg_config")
    p.add_argument("--build-dir", type=Path, required=True, help="new, nonexistent directory")
    p.add_argument("--source", type=Path,
                   help="a local PostGIS clone to take the pinned commit from (default: the "
                        "pin's repository)")
    p.add_argument("--jobs", type=int, default=4)
    p.add_argument("--allow-library-mismatch", action="store_true",
                   help="build with library versions other than the pin's (recorded)")
    args = p.parse_args()
    if args.jobs <= 0:
        p.error("jobs must be positive")
    manifest = postgis.build(pg_config=args.pg_config, build_dir=args.build_dir.resolve(),
                             jobs=args.jobs, source=args.source,
                             allow_library_mismatch=args.allow_library_mismatch)
    print(json.dumps({k: manifest[k] for k in ("commit", "library_versions", "full_version")},
                     indent=1))


if __name__ == "__main__":
    main()
