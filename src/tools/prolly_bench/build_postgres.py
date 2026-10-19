"""Fresh out-of-tree server build, with source and installed-binary hashes."""
import argparse
import os
import subprocess
from pathlib import Path

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
    p.add_argument("configure", nargs=argparse.REMAINDER)
    args = p.parse_args()
    if args.jobs <= 0:
        p.error("jobs must be positive")
    source = args.source.resolve()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=False)
    before = sources(build, source)
    install = build / "inst"
    flags = args.configure[1:] if args.configure[:1] == ["--"] else args.configure
    command = [str(source / "configure"), f"--prefix={install}", *flags]
    with (build / "build.log").open("w") as log:
        # pg_walinspect: the write_amp collector splits WAL by resource manager.
        for cmd in (command, ["make", f"-j{args.jobs}"], ["make", "install"],
                    ["make", "-C", "contrib/pg_walinspect", "install"]):
            subprocess.run(cmd, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    if before != sources(build, source):
        raise RuntimeError("source changed during build; refusing to attest this install")
    binary_hashes = {name: sha256(install / "bin" / name)
                     for name in ("postgres", "pg_config", "psql", "pgbench", "initdb", "pg_ctl")}
    library_hashes = envmod.library_hashes(install / "bin")
    if set(library_hashes) != set(envmod.LIBRARIES):
        raise RuntimeError(f"missing server libraries: {set(envmod.LIBRARIES) - set(library_hashes)}")
    atomic_json(install / "pbench-build.json", {
        "schema_version": 1, "source_files": before, "binary_hashes": binary_hashes,
        "library_hashes": library_hashes,
        "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip(),
        "configure_command": command})
    print(install / "bin")


if __name__ == "__main__":
    main()
