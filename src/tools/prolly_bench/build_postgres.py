"""Fresh out-of-tree server build, with source and installed-binary hashes."""
import argparse
import os
import subprocess
from pathlib import Path

from pbench.core.validation import atomic_json, sha256

REPO = Path(__file__).resolve().parents[3]


def sources(build: Path):
    """The source tree's files and their hashes, but the build's own (a
    build directory inside the tree isn't source)."""
    names = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=REPO).split(b"\0")
    paths = (REPO / os.fsdecode(n) for n in names if n)
    return {str(p.relative_to(REPO)): sha256(p) for p in paths
            if p.is_file() and not p.is_relative_to(build)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build-dir", type=Path, required=True, help="new, nonexistent directory")
    p.add_argument("--jobs", type=int, default=4)
    p.add_argument("configure", nargs=argparse.REMAINDER)
    args = p.parse_args()
    if args.jobs <= 0:
        p.error("jobs must be positive")
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=False)
    before = sources(build)
    install = build / "inst"
    flags = args.configure[1:] if args.configure[:1] == ["--"] else args.configure
    command = [str(REPO / "configure"), f"--prefix={install}", *flags]
    with (build / "build.log").open("w") as log:
        for cmd in (command, ["make", f"-j{args.jobs}"], ["make", "install"]):
            subprocess.run(cmd, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    if before != sources(build):
        raise RuntimeError("source changed during build; refusing to attest this install")
    binary_hashes = {name: sha256(install / "bin" / name)
                     for name in ("postgres", "pg_config", "psql", "pgbench", "initdb", "pg_ctl")}
    atomic_json(install / "pbench-build.json", {
        "schema_version": 1, "source_files": before, "binary_hashes": binary_hashes,
        "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
        "configure_command": command})
    print(install / "bin")


if __name__ == "__main__":
    main()
