"""Shared evidence contract for execution, gates and reports.

Old artifacts remain readable, but cannot establish a valid comparison.
Completion is an atomic manifest with a planned denominator and hashes.
"""
from __future__ import annotations

import hashlib
import json
import math
import shlex
from collections import Counter
from pathlib import Path

SCHEMA_VERSION = 2


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def fingerprint(value) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                     allow_nan=False).encode()).hexdigest()


def atomic_json(path: Path, value) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, indent=2, allow_nan=False, default=str) + "\n")
    tmp.replace(path)


def cell_id(key: dict) -> str:
    return json.dumps(key, sort_keys=True)


def tpch_answers(raw: Path) -> list[dict]:
    answers = [json.loads(line) for line in (raw / "tpch-answers.jsonl").read_text().splitlines()]
    if Counter(r["query"] for r in answers) != Counter({f"Q{i}": 2 for i in range(1, 23)}):
        raise ValueError("TPC-H correctness pass must contain two answers for each of Q1–Q22")
    for r in answers:
        if r["version"] != 1 or type(r["rows"]) is not int or r["rows"] < 0:
            raise ValueError("invalid TPC-H answer record")
        for name in ("sql_sha256", "columns_sha256", "ordered_sha256", "sorted_sha256"):
            if len(r[name]) != 64 or any(c not in "0123456789abcdef" for c in r[name]):
                raise ValueError("invalid TPC-H answer digest")
    return answers


def cell_problems(result: dict) -> list[str]:
    problems = []
    if result.get("status") != "ok":
        problems.append(result.get("status", "missing status"))
    problems.extend(result.get("validation_errors", []))
    for name, value in result.get("metrics", {}).items():
        if value is not None and (type(value) not in (int, float) or not math.isfinite(value)):
            problems.append(f"invalid metric {name}")
    return problems


def validate_run(path: Path) -> list[str]:
    try:
        run = json.loads((path / "run.json").read_text())
        if run.get("schema_version") != SCHEMA_VERSION:
            return ["legacy or unknown result schema; rerun to establish validity"]
        if run.get("state") != "complete":
            return ["run did not complete"]
        planned = run["planned_cells"]
        results = [json.loads(line) for line in (path / "results.jsonl").read_text().splitlines()]
        expected = Counter(cell_id(k) for k in planned)
        actual = Counter(cell_id(r["key"]) for r in results)
        problems = []
        if not planned or any(n != 1 for n in expected.values()) or actual != expected:
            problems.append("missing, duplicate or unexpected cells")
        hashes = run["artifacts"]
        required = {"results.jsonl", "config.json", "environment.json"}
        if not required <= hashes.keys():
            problems.append("missing required artifact hashes")
        for name, digest in hashes.items():
            target = (path / name).resolve()
            if not target.is_relative_to(path.resolve()) or sha256(target) != digest:
                problems.append(f"artifact changed: {name}")
        for r in results:
            problems.extend(f"{cell_id(r['key'])}: {p}" for p in cell_problems(r))
            if r.get("status") == "ok":
                denominator = r.get("denominator")
                seconds = r.get("measurement", {}).get("seconds")
                if (not r.get("unit") or type(denominator) not in (float, int)
                        or not math.isfinite(denominator) or denominator <= 0
                        or type(seconds) not in (float, int) or not math.isfinite(seconds) or seconds <= 0):
                    problems.append("successful cell lacks a positive finite measured window and completed-work denominator")
            if r.get("raw_dir"):
                raw = Path(r["raw_dir"])
                if raw.is_absolute() or not (path / raw).resolve().is_relative_to(path.resolve()):
                    problems.append("raw artifacts must use portable paths within the result directory")
                    continue
            if r["key"].get("workload") == "S13" and r.get("status") == "ok":
                tpch_answers(path / r["raw_dir"])
            if run["suite"] == "C" and r.get("status") == "ok":
                from pbench.ccompare import evidence
                errors, _ = evidence(path / r["raw_dir"] / "slt", run["table_am"])
                problems.extend(errors)
        return problems
    except (OSError, ValueError, KeyError, TypeError) as e:
        return [f"invalid result evidence: {e}"]


def comparison_identity(cfg: dict, env: dict) -> str:
    # AM, source revision and server build (with its psql and pgbench) may
    # differ: these are the independent variables. Their exact hashes are retained as provenance.
    # All workload, durability, build-mode and machine conditions must match.
    return fingerprint({
        "config": {k: v for k, v in cfg.items() if k not in ("source", "tools")},
        "machine": {k: env.get(k) for k in (
            "hostname", "kernel", "memory_kb", "cpu_governors",
            "cpu_energy_preference", "cpu_boost", "filesystem", "block_device")},
        "build_mode": {
            "configure": [arg for arg in shlex.split(env.get("postgres", {}).get("configure", ""))
                          if not arg.startswith(("--prefix=", "--exec-prefix=", "--bindir=",
                                                 "--libdir=", "--includedir=", "--datarootdir=",
                                                 "--docdir=", "--mandir="))],
            "cflags": env.get("postgres", {}).get("cflags"),
        },
        "cpu": {r["field"]: r["data"] for r in env.get("cpu", {}).get("lscpu", [])
                if "MHz" not in r["field"]},
        "harness": env.get("harness_hash"),
        # (psql and pgbench come with the server build, so their hashes
        # differ between builds; kept as provenance)
        "clients": {k: v for k, v in (env.get("client_hashes") or {}).items()
                    if k not in ("psql", "pgbench")},
    })


def compatible(base: Path, other: Path) -> list[str]:
    problems = validate_run(base) + validate_run(other)
    if problems:
        return problems
    b = json.loads((base / "run.json").read_text())
    e = json.loads((other / "run.json").read_text())
    if not b.get("comparison_identity") or b["comparison_identity"] != e.get("comparison_identity"):
        problems.append("different workload, build mode, client, harness or machine conditions")
    if not problems:
        base_results = {cell_id(r["key"]): r for r in
                        (json.loads(line) for line in (base / "results.jsonl").read_text().splitlines())}
        for line in (other / "results.jsonl").read_text().splitlines():
            r = json.loads(line)
            if r["key"].get("workload") == "S13":
                reference = base_results.get(cell_id(r["key"]))
                if reference is None or tpch_answers(other / r["raw_dir"]) != tpch_answers(base / reference["raw_dir"]):
                    problems.append(f"TPC-H query parameters, types, row counts or actual answers differ: {r['key']}")
    return problems
