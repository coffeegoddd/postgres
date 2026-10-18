"""Run configuration: a TOML file plus command-line overrides.

The resolved configuration is written with every result set, so a run can
be repeated exactly.
"""

from __future__ import annotations

import copy
import math
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
    "collectors": ["environment", "wal", "diskstats", "pg_stat_io", "cpu",
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
    "seed": 1,
    "quality": "smoke",  # benchmark requires controlled conditions
    "required_collectors": [],
    # C suite only: "I/N" runs shard I of N of the corpus (cshard.py).
    "shard": "",
    # Counters read after the measured clients have gone (so every backend
    # that did the work has flushed its statistics), pg_stat_io's for client
    # backends only: for runs whose counters are compared exactly.
    "settled_counters": False,
}

# Settings every S and V run requires: results are only comparable when
# durable.  The C suite measures correctness, not speed, and doesn't crash
# the server on purpose, so it may run without them.
REQUIRED_SETTINGS = {"fsync": "on", "synchronous_commit": "on"}
DURABILITY_EXEMPT_SUITES = {"C"}

# The default configuration per suite, in configs/.
DEFAULT_CONFIGS = {"S": "standard.toml", "V": "standard.toml", "C": "c_standard.toml"}

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


def load(path: Path, overrides: dict[str, Any], suite: str = "S") -> dict[str, Any]:
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
    validate(cfg, suite)

    for name in cfg["run"]["sizes"]:
        if name not in cfg["sizes"]:
            raise ConfigError(f"size {name!r} has no [sizes.{name}] section")
    for k, v in REQUIRED_SETTINGS.items():
        if suite in DURABILITY_EXEMPT_SUITES:
            break
        actual = cfg["postgres"].get(k, v)
        if actual != v:
            raise ConfigError(f"{k} = {actual}: the procedure requires {k} = {v} "
                              "(results with it off are not reported)")
        cfg["postgres"][k] = v
    return cfg


def validate(cfg: dict, suite: str) -> None:
    run = cfg["run"]
    def number(value, name, minimum=1, maximum=None):
        if (isinstance(value, bool) or not isinstance(value, (float, int))
                or not math.isfinite(value) or value < minimum
                or maximum is not None and value > maximum):
            raise ConfigError(f"invalid {name}: {value!r}")
    for name in ("repetitions", "fio_seconds"):
        number(run[name], name)
        if type(run[name]) is not int:
            raise ConfigError(f"{name} must be an integer")
    for name in ("warmup_seconds", "measure_seconds", "seed"):
        number(run[name], name, 0 if name != "measure_seconds" or suite == "C" else 1)
        if type(run[name]) is not int:
            raise ConfigError(f"{name} must be an integer")
    number(run["latency_sampling_rate"], "latency_sampling_rate", 1e-12, 1)
    if run["shard"]:
        if suite != "C":
            raise ConfigError("only the C suite runs in shards")
        from pbench import cshard
        try:
            cshard.parse(str(run["shard"]))
        except ValueError as e:
            raise ConfigError(str(e)) from None
    if run["seed"] + run["repetitions"] > 2147483647:
        raise ConfigError("seed plus repetitions must fit BenchBase's positive 32-bit seed")
    for name in ("sizes", "clients", "sinks"):
        if not isinstance(run[name], list) or not run[name] or len(set(run[name])) != len(run[name]):
            raise ConfigError(f"{name} must be a nonempty list without duplicates")
    for client in run["clients"]:
        number(client, "clients")
        if type(client) is not int:
            raise ConfigError("clients must be integers")
    if "files" not in run["sinks"]:
        raise ConfigError("files sink is required for verifiable results")
    if run["quality"] not in ("smoke", "benchmark"):
        raise ConfigError("quality must be smoke or benchmark")
    if run["quality"] == "benchmark" and (run["repetitions"] < 5 or run["warmup_seconds"] < 1):
        raise ConfigError("benchmark quality requires at least 5 repetitions and warmup")
    if run["quality"] == "benchmark" and "environment" not in run["collectors"]:
        raise ConfigError("benchmark quality requires runtime environment monitoring")
    if type(run["settled_counters"]) is not bool:
        raise ConfigError("settled_counters must be true or false")
    if not set(run["required_collectors"]) <= set(run["collectors"]):
        raise ConfigError("required_collectors must be enabled")
    for label, sizes in cfg["sizes"].items():
        for name, value in sizes.items():
            if name not in {"pgbench_scale", "s4_rows", "s7_rows", "tpcc_warehouses", "tpch_scale",
                            "v_rows", "sysbench_tables", "sysbench_table_size"}:
                raise ConfigError(f"unknown size parameter {label}.{name}")
            number(value, f"sizes.{label}.{name}", 1e-12)
            if name != "tpch_scale" and type(value) is not int:
                raise ConfigError(f"sizes.{label}.{name} must be an integer")
    common = {"clients", "warmup_seconds", "measure_seconds", "protocol",
              "perf_frequency", "hash_symbols", "latency_sampling_rate", "timeout_seconds",
              "rate", "latency_limit_ms", "transactions"}
    keys = {"S3": {"ranges"}, "S5": {"hot_rows"}, "S8": {"percents"},
            "S9": {"isolation", "weights"}, "S10": {"tests"}, "S13": {"modes", "weights", "isolation"},
            "V1": {"iterations", "background"}, "V2": {"iterations", "background", "changes"},
            "V3": {"background", "changes", "locality", "conflict_percent"}, "V4": {"background", "changes"},
            "V6": {"background", "branches", "changes"}}
    keys.update({f"C{i}": {"workers", "sample"} for i in range(1, 5)})
    keys.update({f"S{i}": set() for i in (1, 2, 4, 6, 7)})
    for wid, params in cfg["workloads"].items():
        if wid not in keys or set(params) - (keys[wid] | common):
            raise ConfigError(f"unknown workload or parameters for {wid}: {sorted(params)}")
        for name, choices in {"sample": {"all", "per_directory"},
                              "protocol": {"simple", "extended", "prepared"}}.items():
            if name in params and params[name] not in choices:
                raise ConfigError(f"invalid {wid}.{name}")
        for name, choices in {"modes": {"serial", "throughput"},
                              "background": {"none", "S1"},
                              "locality": {"clustered", "scattered"}}.items():
            if name in params and (not params[name] or not set(params[name]) <= choices):
                raise ConfigError(f"invalid {wid}.{name}")
        for name in ("clients", "ranges", "changes", "percents", "conflict_percent", "branches"):
            if name in params:
                if not params[name] or len(set(params[name])) != len(params[name]):
                    raise ConfigError(f"empty or duplicate {wid}.{name}")
                for v in params[name]:
                    number(v, f"{wid}.{name}", 0 if name == "conflict_percent" else 1,
                           100 if name in ("percents", "conflict_percent") else None)
                    if name in ("clients", "ranges", "changes", "branches") and type(v) is not int:
                        raise ConfigError(f"{wid}.{name} must contain integers")
        for name in ("iterations", "workers", "hot_rows", "timeout_seconds", "measure_seconds",
                     "warmup_seconds", "transactions"):
            if name in params:
                number(params[name], f"{wid}.{name}", 0 if name == "warmup_seconds" else 1)
                if type(params[name]) is not int:
                    raise ConfigError(f"{wid}.{name} must be an integer")
        if "transactions" in params and wid not in {"S1", "S2", "S3", "S5", "S6", "S7", "S8", "S10"}:
            raise ConfigError("transactions is supported only by pgbench and sysbench workloads")
        for name in ("rate", "latency_limit_ms"):
            if name in params:
                if wid not in {"S1", "S2", "S3", "S5", "S6", "S7", "S8"}:
                    raise ConfigError(f"{name} is supported only by pgbench workloads")
                number(params[name], f"{wid}.{name}", 1e-12)


def server_settings(cfg: dict[str, Any]) -> dict[str, str]:
    return {**HARNESS_SETTINGS, **cfg["postgres"]}
