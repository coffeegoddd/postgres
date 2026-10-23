"""PostGIS built against a just-installed server, with provenance.

configs/postgis.toml pins the PostGIS commit and the GEOS, PROJ and GDAL
versions it is built with.  Those libraries come from the system (they are
packages, not this tree's), so the pin is a check: a build that finds
another version is refused unless told otherwise, and the versions found,
the shared objects PostGIS links and their hashes are recorded either way.

A build:

1. clones the pinned commit (from the repository, or a local checkout)
   into BUILD/src and checks it is clean and at the pin;
2. finds the library versions and checks them against the pin;
3. runs autogen.sh and configure against the server's pg_config (raster
   and topology always, SFCGAL when sfcgal-config is found), make, and
   make install into the server's installation;
4. starts a throwaway cluster of that installation, creates every
   extension and reads postgis_full_version(), which names the library
   versions PostGIS itself sees;
5. writes the manifest: commit, configure command, library versions,
   linked libraries and their hashes, the installed modules' hashes, the
   extensions' versions and postgis_full_version().

build_postgres.py --with-postgis does this after building the server and
puts the manifest in the server's pbench-build.json, whose library list
then includes PostGIS's modules (so every cell rechecks them).
"""

from __future__ import annotations

import json
import os
import re
import shutil
import socket
import subprocess
import tempfile
import tomllib
from pathlib import Path
from typing import Any, Callable

from pbench.core.validation import atomic_json, sha256

HERE = Path(__file__).resolve().parents[1]
PIN = HERE / "configs" / "postgis.toml"

# The extensions a build installs, created in this order (each needs the
# ones before it).
EXTENSIONS = ("postgis", "postgis_raster", "postgis_topology")
SFCGAL_EXTENSION = "postgis_sfcgal"

# Modules PostGIS installs into pkglibdir; the SFCGAL one only when built
# with it.
MODULES = ("postgis-3.so", "postgis_raster-3.so", "postgis_topology-3.so")
SFCGAL_MODULE = "postgis_sfcgal-3.so"

# Shared objects whose versions the build records, by the name ldd shows.
LINKED = re.compile(r"lib(geos_c|geos|proj|gdal|SFCGAL|json-c|protobuf-c|xml2)\.so")


class PinError(RuntimeError):
    pass


def read_pin(path: Path = PIN) -> dict[str, Any]:
    with open(path, "rb") as f:
        pin = tomllib.load(f)
    unknown = set(pin) - {"repository", "commit", "configure", "libraries"}
    if unknown:
        raise PinError(f"{path}: unknown keys {sorted(unknown)}")
    if not re.fullmatch(r"[0-9a-f]{40}", pin.get("commit", "")):
        raise PinError(f"{path}: commit must be a full 40-digit SHA")
    libs = pin.get("libraries", {})
    if set(libs) - {"geos", "proj", "gdal", "sfcgal"} or not {"geos", "proj", "gdal"} <= set(libs):
        raise PinError(f"{path}: [libraries] pins geos, proj and gdal (and optionally sfcgal)")
    if not all(isinstance(v, str) and re.fullmatch(r"[0-9][0-9.]*", v) for v in libs.values()):
        raise PinError(f"{path}: library versions are dotted numbers, as strings")
    pin.setdefault("configure", [])
    return pin


def _out(args: list[str]) -> str:
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return r.stdout.strip() if r.returncode == 0 else ""


def library_versions(out: Callable[[list[str]], str] = _out) -> dict[str, str | None]:
    """The version of each library PostGIS builds with, as its config tool
    or pkg-config reports it; None for one not found."""
    def first(*commands):
        for c in commands:
            v = out(c)
            if v:
                m = re.search(r"[0-9]+(\.[0-9]+)+", v)
                return m.group(0) if m else v
        return None
    return {
        "geos": first(["geos-config", "--version"], ["pkg-config", "--modversion", "geos"]),
        "proj": first(["pkg-config", "--modversion", "proj"]),
        "gdal": first(["gdal-config", "--version"], ["pkg-config", "--modversion", "gdal"]),
        "sfcgal": first(["sfcgal-config", "--version"], ["pkg-config", "--modversion", "sfcgal"]),
        "json-c": first(["pkg-config", "--modversion", "json-c"]),
        "protobuf-c": first(["pkg-config", "--modversion", "libprotobuf-c"]),
        "libxml2": first(["pkg-config", "--modversion", "libxml-2.0"], ["xml2-config", "--version"]),
    }


def library_mismatches(pin: dict[str, Any], found: dict[str, str | None]) -> list[str]:
    """Each pinned library the build would use at another version (or
    not at all)."""
    out = []
    for name, want in sorted(pin["libraries"].items()):
        have = found.get(name)
        if have != want:
            out.append(f"{name} {have or 'not found'}, pinned {want}")
    return out


def full_version_fields(text: str) -> dict[str, str]:
    """postgis_full_version()'s NAME="value" fields, each value its
    version: PROJ's settings (NAME=value words, a per-user directory among
    them) and GDAL's own name left out."""
    return {k: " ".join(w for w in v.split() if "=" not in w and w != "GDAL")
            for k, v in re.findall(r'([A-Z_]+)="([^"]*)"', text)}


def full_version_mismatches(recorded: str, now: str) -> list[str]:
    """Fields of postgis_full_version() that differ between the build's
    record and a running server (a library replaced under the build)."""
    a, b = full_version_fields(recorded), full_version_fields(now)
    return [f"{k}: built with {a.get(k) or 'none'}, running {b.get(k) or 'none'}"
            for k in sorted(set(a) | set(b)) if a.get(k) != b.get(k)]


def linked_libraries(module: Path, out: Callable[[list[str]], str] = _out) -> dict[str, dict[str, str]]:
    """The geospatial shared objects a module links, by soname: path and
    hash."""
    found = {}
    for line in out(["ldd", str(module)]).splitlines():
        m = re.match(r"\s*(\S+)\s+=>\s+(\S+)", line)
        if m and LINKED.match(m.group(1)) and Path(m.group(2)).is_file():
            path = Path(m.group(2)).resolve()
            found[m.group(1)] = {"path": str(path), "sha256": sha256(path)}
    return found


def linked_by(pkglibdir: Path, names: tuple[str, ...],
              out: Callable[[list[str]], str] = _out) -> dict[str, dict[str, str]]:
    """The geospatial shared objects any of the modules links (SFCGAL's
    only postgis_sfcgal's)."""
    found: dict[str, dict[str, str]] = {}
    for name in names:
        found.update(linked_libraries(pkglibdir / name, out))
    return dict(sorted(found.items()))


def preflight(pin_path: Path = PIN, allow_library_mismatch: bool = False,
              versions: Callable[[], dict[str, str | None]] | None = None):
    """The pin, the library versions found and their mismatches, refused
    before anything is built when they differ and that isn't allowed."""
    pin = read_pin(pin_path)
    found = (versions or library_versions)()
    mismatches = library_mismatches(pin, found)
    if mismatches and not allow_library_mismatch:
        raise PinError("PostGIS's libraries don't match configs/postgis.toml: " + "; ".join(mismatches)
                       + " (install the pinned versions, update the pin, or pass "
                         "--postgis-allow-library-mismatch, which is recorded)")
    return pin, found, mismatches


def pg_config_value(pg_config: Path, flag: str) -> Path:
    return Path(subprocess.check_output([str(pg_config), flag], text=True).strip())


def modules(sfcgal: bool) -> tuple[str, ...]:
    return MODULES + ((SFCGAL_MODULE,) if sfcgal else ())


def extensions(sfcgal: bool) -> tuple[str, ...]:
    return EXTENSIONS + ((SFCGAL_EXTENSION,) if sfcgal else ())


def _run(cmd: list[str], cwd: Path, log) -> None:
    log.write(f"$ {' '.join(cmd)}\n")
    log.flush()
    subprocess.run(cmd, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)


def _source(build: Path, pin: dict[str, Any], source: Path | None, log) -> Path:
    src = build / "src"
    origin = str(source.resolve()) if source else pin["repository"]
    _run(["git", "clone", "--quiet", "--no-checkout", origin, str(src)], build, log)
    _run(["git", "checkout", "--quiet", "--detach", pin["commit"]], src, log)
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=src, text=True).strip()
    if head != pin["commit"]:
        raise PinError(f"PostGIS source is at {head}, not the pinned {pin['commit']}")
    if subprocess.check_output(["git", "status", "--porcelain"], cwd=src, text=True).strip():
        raise PinError("PostGIS source has local changes")
    return src


def readings(sql: Callable[[str], str], sfcgal: bool) -> dict[str, Any]:
    """postgis_full_version() with the extensions a G or GV database
    creates (what each cell's reading is compared with: it names SFCGAL
    only once postgis_sfcgal exists), then postgis_sfcgal created too when
    built, and every extension's version."""
    for name in EXTENSIONS:
        sql(f"CREATE EXTENSION {name}")
    out: dict[str, Any] = {"full_version": sql("SELECT postgis_full_version()")}
    if sfcgal:
        sql(f"CREATE EXTENSION {SFCGAL_EXTENSION}")
        out["full_version_with_sfcgal"] = sql("SELECT postgis_full_version()")
    out["extension_versions"] = json.loads(sql(
        "SELECT json_object_agg(extname, extversion ORDER BY extname) FROM pg_extension "
        "WHERE extname <> 'plpgsql'"))
    return out


def _smoke(bin_dir: Path, work: Path, sfcgal: bool) -> dict[str, Any]:
    """readings() in a throwaway cluster."""
    data = work / "data"
    env = {**os.environ, "PGTZ": "UTC", "LC_ALL": "C"}
    subprocess.run([str(bin_dir / "initdb"), "-D", str(data), "-A", "trust", "-U", "postgres",
                    "--no-sync"], check=True, capture_output=True, env=env)
    with socket.socket() as s:
        s.bind(("localhost", 0))
        port = s.getsockname()[1]
    # TCP only: no socket directory, whose path length is limited.
    subprocess.run([str(bin_dir / "pg_ctl"), "-D", str(data), "-w", "-l", str(work / "server.log"),
                    "-o", f"-p {port} -k '' -c listen_addresses=localhost -c fsync=off", "start"],
                   check=True, capture_output=True, env=env)
    try:
        def sql(q: str) -> str:
            return subprocess.check_output(
                [str(bin_dir / "psql"), "-X", "-q", "-At", "-v", "ON_ERROR_STOP=1", "-h", "localhost",
                 "-p", str(port), "-U", "postgres", "-d", "postgres", "-c", q],
                text=True, env=env, stderr=subprocess.STDOUT).strip()
        return readings(sql, sfcgal)
    finally:
        subprocess.run([str(bin_dir / "pg_ctl"), "-D", str(data), "-m", "fast", "-w", "stop"],
                       capture_output=True, env=env)


def build(*, pg_config: Path, build_dir: Path, jobs: int = 4, source: Path | None = None,
          pin_path: Path = PIN, allow_library_mismatch: bool = False) -> dict[str, Any]:
    """Build and install the pinned PostGIS against pg_config's
    installation; the manifest (also written to BUILD/pbench-postgis.json)."""
    pin, found, mismatches = preflight(pin_path, allow_library_mismatch)
    pg_config = pg_config.resolve()
    build_dir.mkdir(parents=True, exist_ok=False)
    sfcgal = found.get("sfcgal") is not None
    bin_dir = pg_config_value(pg_config, "--bindir")
    pkglibdir = pg_config_value(pg_config, "--pkglibdir")
    # PostGIS's own installs outside pg_config's directories (its man
    # pages) go under the server's prefix, never the system's.
    prefix = bin_dir.parent
    configure = ["./configure", f"--with-pgconfig={pg_config}", f"--prefix={prefix}",
                 "--with-raster", "--with-topology",
                 "--with-sfcgal" if sfcgal else "--without-sfcgal", *pin["configure"]]
    with (build_dir / "build.log").open("w") as log:
        src = _source(build_dir, pin, source, log)
        for cmd in (["./autogen.sh"], configure, ["make", f"-j{jobs}"], ["make", "install"]):
            _run(cmd, src, log)
    names = modules(sfcgal)
    missing = [n for n in names if not (pkglibdir / n).is_file()]
    if missing:
        raise RuntimeError(f"PostGIS install lacks {missing}")
    work = Path(tempfile.mkdtemp(prefix="smoke-", dir=build_dir))
    try:
        smoke = _smoke(bin_dir, work, sfcgal)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    manifest = {
        "schema_version": 1,
        "repository": pin["repository"], "commit": pin["commit"],
        "source": str(source.resolve()) if source else pin["repository"],
        "configure_command": configure,
        "pinned_libraries": pin["libraries"], "library_versions": found,
        "library_mismatches": mismatches,
        "linked_libraries": linked_by(pkglibdir, names),
        "module_hashes": {n: sha256(pkglibdir / n) for n in names},
        "extensions": list(extensions(sfcgal)),
        **smoke,
    }
    atomic_json(build_dir / "pbench-postgis.json", manifest)
    return manifest
