"""Recording the environment of a run: machine, kernel, CPU settings,
filesystem, device, build, and an fio baseline of the device.  Also produces
warnings for conditions that make results less trustworthy."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import socket
import subprocess
from pathlib import Path
from typing import Any
from pbench.core.validation import sha256, fingerprint

STATE_FILE = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) \
    / "prolly_bench" / "system_settings.saved"


def _cmd(args: list[str], cwd: Path | None = None) -> str:
    try:
        r = subprocess.run(args, capture_output=True, text=True, cwd=cwd, timeout=60)
        return r.stdout.strip() if r.returncode == 0 else ""
    except (OSError, subprocess.TimeoutExpired):
        return ""


def _read(path: str) -> str:
    try:
        return Path(path).read_text().strip()
    except OSError:
        return ""


def _git(repo: Path) -> dict[str, Any]:
    if not repo.exists():
        return {"path": str(repo), "present": False}
    return {"path": str(repo),
            "commit": _cmd(["git", "rev-parse", "HEAD"], repo),
            "branch": _cmd(["git", "rev-parse", "--abbrev-ref", "HEAD"], repo),
            "dirty_files": len(_cmd(["git", "status", "--porcelain"], repo).splitlines()),
            "diff_sha256": hashlib.sha256(_cmd(["git", "diff", "HEAD", "--binary"], repo).encode()).hexdigest()}


def record(*, repo: Path, bin_dir: Path, data_root: Path, tools: dict[str, str]) -> dict[str, Any]:
    cpus = sorted(Path("/sys/devices/system/cpu").glob("cpu[0-9]*/cpufreq"))
    governors = sorted({_read(str(c / "scaling_governor")) for c in cpus})
    epp = sorted({_read(str(c / "energy_performance_preference")) for c in cpus})
    mount = _cmd(["findmnt", "-J", "-T", str(data_root)])
    source = ""
    try:
        source = json.loads(mount)["filesystems"][0]["source"].split("[")[0]
    except (ValueError, KeyError, IndexError):
        pass
    pg_config = str(bin_dir / "pg_config")
    env: dict[str, Any] = {
        "hostname": socket.gethostname(),
        "kernel": platform.uname()._asdict(),
        "os": dict(line.split("=", 1) for line in _read("/etc/os-release").splitlines()
                   if "=" in line),
        "cpu": json.loads(_cmd(["lscpu", "-J"]) or "{}"),
        "memory_kb": next((int(line.split()[1]) for line in _read("/proc/meminfo").splitlines()
                           if line.startswith("MemTotal")), None),
        "cpu_governors": governors,
        "cpu_energy_preference": epp,
        "cpu_boost": _read("/sys/devices/system/cpu/cpufreq/boost"),
        "perf_event_paranoid": _read("/proc/sys/kernel/perf_event_paranoid"),
        "system_prepare_applied": STATE_FILE.exists(),
        "loadavg_at_start": _read("/proc/loadavg"),
        "data_root": str(data_root),
        "data_root_attributes": _cmd(["lsattr", "-d", str(data_root)]),
        "filesystem": json.loads(mount or "{}"),
        "block_device": json.loads(_cmd(["lsblk", "-J", "-s", "-o",
                                         "NAME,MODEL,SIZE,ROTA,TYPE,SCHED,DISC-GRAN",
                                         source]) or "{}") if source else {},
        "postgres": {
            "bin_dir": str(bin_dir),
            "version": _cmd([pg_config, "--version"]),
            "configure": _cmd([pg_config, "--configure"]),
            "cflags": _cmd([pg_config, "--cflags"]),
        },
        "repo": _git(repo),
        "python": platform.python_version(),
        "tools": {t: shutil.which(t) for t in ["fio", "perf", "java", "pgbench"]},
    }
    if tools.get("benchbase_source"):
        env["benchbase_source"] = _git(Path(tools["benchbase_source"]))
    if tools.get("benchbase_build_commit_file"):
        env["benchbase_build_commit"] = _read(tools["benchbase_build_commit_file"])
    if tools.get("sqllogictest"):
        env["sqllogictest"] = _git(Path(tools["sqllogictest"]))
    if tools.get("slt_source_hash_file"):
        env["slt_build_source_hash"] = _read(tools["slt_source_hash_file"])
        env["slt_source_hash"] = slt_source_hash(repo)
    env["postgres"]["binary_hashes"] = {
        name: sha256(bin_dir / name) for name in ("postgres", "pg_config", "psql", "pgbench", "initdb", "pg_ctl")
        if (bin_dir / name).is_file()}
    provenance = bin_dir.parent / "pbench-build.json"
    env["postgres"]["build_provenance"] = json.loads(provenance.read_text()) if provenance.exists() else None
    src = repo / "src/tools/prolly_bench"
    env["harness_hash"] = fingerprint({str(p.relative_to(src)): sha256(p)
        for p in sorted(src.rglob("*")) if p.is_file() and p.suffix in (".py", ".go", ".sql", ".sh", ".java")})
    env["client_hashes"] = {name: env["postgres"]["binary_hashes"].get(name)
                            for name in ("pgbench", "psql")}
    for name in ("java", "sysbench"):
        executable = shutil.which(name)
        if executable:
            env["client_hashes"][name] = sha256(Path(executable))
    if tools.get("benchbase"):
        dist = Path(tools["benchbase"])
        env["client_hashes"]["benchbase"] = fingerprint({str(p.relative_to(dist)): sha256(p)
            for p in sorted(dist.rglob("*.jar"))})
    if tools.get("slt_runner") and Path(tools["slt_runner"]).is_file():
        env["client_hashes"]["slt_runner"] = sha256(Path(tools["slt_runner"]))
    return env


def slt_source_hash(repo: Path) -> str:
    """The hash build_slt.sh records: go.mod, go.sum and *.go of the
    runner's source, concatenated in byte order of their names."""
    src = repo / "src" / "tools" / "prolly_bench" / "slt"
    names = sorted(p.name for p in src.iterdir()
                   if p.name in ("go.mod", "go.sum") or p.suffix == ".go")
    h = hashlib.sha256()
    for n in names:
        h.update((src / n).read_bytes())
    return h.hexdigest()


def warnings(env: dict[str, Any], suite: str = "S") -> list[str]:
    w = []
    if env.get("slt_build_source_hash") is not None and \
            env["slt_build_source_hash"] != env.get("slt_source_hash"):
        w.append("slt-runner was built from other source than slt/ holds now; "
                 "run build_slt.sh")
    if suite == "C":
        return w  # the rest concern performance
    provenance = env["postgres"].get("build_provenance")
    if not provenance or provenance.get("binary_hashes") != env["postgres"].get("binary_hashes"):
        w.append("server binary has no matching build provenance; use build_postgres.sh")
    if env["cpu_governors"] != ["performance"]:
        w.append(f"CPU governor is {env['cpu_governors']}, not performance "
                 "(run system/system_prepare.sh)")
    if env["cpu_boost"] == "1":
        w.append("CPU boost is on; clock speed varies between repetitions")
    elif env["cpu_boost"] != "0":
        w.append("cannot verify that CPU boost is disabled")
    if "--enable-cassert" in env["postgres"]["configure"]:
        w.append("Postgres was built with --enable-cassert; not valid for benchmarking")
    if "-O0" in env["postgres"]["cflags"] or "-O2" not in env["postgres"]["cflags"]:
        w.append(f"Postgres CFLAGS are not -O2: {env['postgres']['cflags']}")
    try:
        fs = env["filesystem"]["filesystems"][0]
        if fs["fstype"] in ("tmpfs", "ramfs", "overlay"):
            w.append(f"data directory uses {fs['fstype']}; not a controlled storage benchmark")
        if fs["fstype"] == "btrfs":
            w.append("data directory is on btrfs: device bytes include a large "
                     "per-fsync filesystem cost; compare WAL "
                     "bytes for write volume")
            if "C" not in env["data_root_attributes"].split()[0]:
                w.append("data directory is on btrfs with copy-on-write on")
            elif "compress" in fs.get("options", ""):
                w.append("btrfs compression is on for the mount (NOCOW files are "
                         "not compressed, so the data directory is unaffected)")
    except (KeyError, IndexError):
        w.append("could not determine the data directory's filesystem")
    load1 = float(env["loadavg_at_start"].split()[0] or 0)
    if load1 > 1.0:
        w.append(f"machine was not idle at start (load average {load1})")
    if env["repo"].get("dirty_files"):
        w.append(f"source tree has {env['repo']['dirty_files']} uncommitted changes")
    bb = env.get("benchbase_source")
    if bb and env.get("benchbase_build_commit") and \
            bb.get("commit") != env["benchbase_build_commit"]:
        w.append("BenchBase build is not from the current commit of its source "
                 f"({env['benchbase_build_commit'][:10]} vs {bb.get('commit', '')[:10]})")
    return w


# ---------------------------------------------------------------------------
# fio baseline
# ---------------------------------------------------------------------------

_FIO_JOBS = {
    # name: (rw, bs, extra)
    "randwrite_4k": ("randwrite", "4k", ["--iodepth=32", "--direct=1",
                                         "--ioengine=io_uring"]),
    "seqwrite_1m": ("write", "1m", ["--iodepth=8", "--direct=1",
                                    "--ioengine=io_uring"]),
    # Like WAL: 8 KB sequential writes, each followed by fdatasync.
    "fsync_8k": ("write", "8k", ["--ioengine=sync", "--fdatasync=1"]),
}


def fio_baseline(data_root: Path, seconds: int, raw_dir: Path) -> dict[str, Any]:
    if shutil.which("fio") is None:
        return {"status": "unavailable", "reason": "fio is not installed"}
    out: dict[str, Any] = {"status": "ok", "seconds_per_job": seconds}
    target = data_root / "fio.test"
    try:
        for name, (rw, bs, extra) in _FIO_JOBS.items():
            args = ["fio", f"--name={name}", f"--filename={target}", f"--rw={rw}",
                    f"--bs={bs}", "--size=2g", f"--runtime={seconds}", "--time_based",
                    "--output-format=json", *extra]
            r = subprocess.run(args, capture_output=True, text=True)
            (raw_dir / f"fio-{name}.json").write_text(r.stdout + r.stderr)
            if r.returncode != 0:
                out[name] = {"error": r.stderr.strip()[-500:]}
                continue
            job = json.loads(r.stdout)["jobs"][0]["write"]
            res = {"iops": job["iops"], "bw_bytes": job["bw_bytes"],
                   "lat_p50_us": job["clat_ns"]["percentile"].get("50.000000", 0) / 1000,
                   "lat_p99_us": job["clat_ns"]["percentile"].get("99.000000", 0) / 1000}
            if name == "fsync_8k":
                sync = json.loads(r.stdout)["jobs"][0]["sync"]["lat_ns"]
                res["fsync_mean_us"] = sync["mean"] / 1000
                res["fsync_p99_us"] = sync.get("percentile", {}).get("99.000000", 0) / 1000
            out[name] = res
    finally:
        target.unlink(missing_ok=True)
    return out
