"""Run configuration: a TOML file plus command-line overrides.

The resolved configuration is written with every result set, so a run can
be repeated exactly.
"""

from __future__ import annotations

import copy
import tomllib
from pathlib import Path
from typing import Any

RUN_DEFAULTS: dict[str, Any] = {
    "repetitions": 5,
    "clients": [1, 4, 16, 64, 256],
    "warmup_seconds": 60,
    "measure_seconds": 600,
    "sizes": ["small"],
    "workloads": [],            # empty: every workload of the suite
    "collectors": ["wal", "diskstats", "pg_stat_io", "cpu",
                   "space", "engine_counters", "perf", "vacuum", "recovery"],
    "sinks": ["console", "files"],
    "cluster": "local",
    # Fraction of transactions whose latency pgbench logs (to RAM).  Lower
    # it for long, high-throughput windows; percentiles stay accurate as
    # long as enough transactions are sampled.
    "latency_sampling_rate": 1.0,
    # Run fio against the data directory's device before the suite.
    "fio_baseline": True,
    "fio_seconds": 20,
}

# Settings every run requires: results are only comparable when durable.
REQUIRED_SETTINGS = {"fsync": "on", "synchronous_commit": "on"}

# Settings the harness itself needs, under the config's settings.
HARNESS_SETTINGS = {
    "log_line_prefix": "'%m [%p] %e '",   # %e: SQLSTATE, to classify errors
    "track_io_timing": "on",
    "track_wal_io_timing": "on",
    "listen_addresses": "'localhost'",   # BenchBase (JDBC) needs TCP
}

_TOP_LEVEL = {"run", "postgres", "sizes", "workloads", "tools"}


class ConfigError(Exception):
    pass


def merge(base: dict, over: dict) -> dict:
    out = copy.deepcopy(base)
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = merge(out[k], v)
        else:
            out[k] = copy.deepcopy(v)
    return out


def load(path: Path, overrides: dict[str, Any]) -> dict[str, Any]:
    with open(path, "rb") as f:
        raw = tomllib.load(f)
    unknown = set(raw) - _TOP_LEVEL
    if unknown:
        raise ConfigError(f"{path}: unknown sections {sorted(unknown)}")
    unknown = set(raw.get("run", {})) - set(RUN_DEFAULTS)
    if unknown:
        raise ConfigError(f"{path}: unknown [run] keys {sorted(unknown)}")

    cfg = {
        "run": merge(RUN_DEFAULTS, raw.get("run", {})),
        "postgres": {k: str(v) for k, v in raw.get("postgres", {}).items()},
        "sizes": raw.get("sizes", {}),
        "workloads": raw.get("workloads", {}),
        "tools": raw.get("tools", {}),
        "source": str(path),
    }
    cfg = merge(cfg, overrides)

    for name in cfg["run"]["sizes"]:
        if name not in cfg["sizes"]:
            raise ConfigError(f"size {name!r} has no [sizes.{name}] section")
    for k, v in REQUIRED_SETTINGS.items():
        actual = cfg["postgres"].get(k, v)
        if actual != v:
            raise ConfigError(f"{k} = {actual}: the procedure requires {k} = {v} "
                              "(results with it off are not reported)")
        cfg["postgres"][k] = v
    return cfg


def server_settings(cfg: dict[str, Any]) -> dict[str, str]:
    return {**HARNESS_SETTINGS, **cfg["postgres"]}
