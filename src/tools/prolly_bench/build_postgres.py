"""Fresh out-of-tree server build, with source and installed-binary hashes.

With --with-postgis, also the pinned PostGIS (configs/postgis.toml) built
against it, recorded under "extensions" in the same manifest, and its
modules added to the libraries every cell rechecks."""
import argparse
import os
import subprocess
from pathlib import Path

from pbench import postgis
from pbench.core import env as envmod
from pbench.core.validation import atomic_json, sha256

REPO = Path(__file__).resolve().parents[3]


def sources(build: Path, source: Path | None = None):
    """The source tree's files (this one's, by default) and their hashes,
    but the build's own (a build directory inside the tree isn't source)."""
    source = source or REPO
    names = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=source).split(b"\0")
    paths = (source / os.fsdecode(n) for n in names if n)
    return {str(p.relative_to(source)): sha256(p) for p in paths
            if p.is_file() and not p.is_relative_to(build)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build-dir", type=Path, required=True, help="new, nonexistent directory")
    p.add_argument("--source", type=Path, default=REPO,
                   help="the source tree to build (default: this one), e.g. a pull "
                        "request's base checked out beside it")
    p.add_argument("--jobs", type=int, default=4)
    p.add_argument("--with-postgis", action="store_true",
                   help="also build the pinned PostGIS against the new install (G and GV suites)")
    p.add_argument("--postgis-source", type=Path,
                   help="a local PostGIS clone to take the pinned commit from (default: the "
                        "pin's repository)")
    p.add_argument("--postgis-allow-library-mismatch", action="store_true",
                   help="build PostGIS with GEOS/PROJ/GDAL versions other than the pin's (recorded)")
    p.add_argument("--with-test-prolly", action="store_true",
                   help="also install the test_prolly module, whose chunk-store functions "
                        "V11 drives (retires, generations, conjoins)")
    p.add_argument("configure", nargs=argparse.REMAINDER)
    args = p.parse_args()
    if args.jobs <= 0:
        p.error("jobs must be positive")
    if args.with_postgis:
        # (a bad pin or other libraries refused before the server's build)
        postgis.preflight(allow_library_mismatch=args.postgis_allow_library_mismatch)
    source = args.source.resolve()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=False)
    before = sources(build, source)
    install = build / "inst"
    flags = args.configure[1:] if args.configure[:1] == ["--"] else args.configure
    command = [str(source / "configure"), f"--prefix={install}", *flags]
    with (build / "build.log").open("w") as log:
        # pg_walinspect: the write_amp collector splits WAL by resource manager.
        steps = [command, ["make", f"-j{args.jobs}"], ["make", "install"],
                 ["make", "-C", "contrib/pg_walinspect", "install"]]
        if args.with_test_prolly:
            steps.append(["make", f"-j{args.jobs}", "-C", "src/test/modules/test_prolly", "install"])
        for cmd in steps:
            subprocess.run(cmd, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    libraries = list(envmod.LIBRARIES)
    extensions = {}
    if args.with_postgis:
        extensions["postgis"] = postgis.build(
            pg_config=install / "bin" / "pg_config", build_dir=build / "postgis", jobs=args.jobs,
            source=args.postgis_source, allow_library_mismatch=args.postgis_allow_library_mismatch)
        libraries += list(extensions["postgis"]["module_hashes"])
    if before != sources(build, source):
        raise RuntimeError("source changed during build; refusing to attest this install")
    binary_hashes = {name: sha256(install / "bin" / name)
                     for name in ("postgres", "pg_config", "psql", "pgbench", "initdb", "pg_ctl")}
    library_hashes = envmod.library_hashes(install / "bin", libraries)
    if set(library_hashes) != set(libraries):
        raise RuntimeError(f"missing server libraries: {set(libraries) - set(library_hashes)}")
    atomic_json(install / "pbench-build.json", {
        "schema_version": 1, "source_files": before, "binary_hashes": binary_hashes,
        "libraries": libraries, "library_hashes": library_hashes, "extensions": extensions,
        "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip(),
        "configure_command": command})
    print(install / "bin")


if __name__ == "__main__":
    main()
