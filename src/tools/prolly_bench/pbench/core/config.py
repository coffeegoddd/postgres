"""Run configuration: a TOML file plus command-line overrides.

The resolved configuration is written with every result set, so a run can
be repeated exactly.
"""

from __future__ import annotations

import copy
import math
import re
import tomllib
from pathlib import Path
from typing import Any

from pbench.core import suites

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
    # A run whose numbers only compare two builds of the server (a pull
    # request and its base) and are never reported as the procedure's: it
    # may turn synchronous_commit off (fsync stays on), so a machine's
    # flush time doesn't bury the change compared.
    "comparison_only": False,
    # What the write_amp collector's flush has prolly do after the window,
    # so its deferred tree writes are counted: "prolly_checkpoint" folds the
    # working set into trees, "none" doesn't (the V suite's workloads
    # commit), and "auto" is the first for S, the second for V.
    "write_amp_materialize": "auto",
}

# Settings every durable suite's run requires (suites.Suite.durable):
# results are only comparable when durable.
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


def load(path: Path, overrides: dict[str, Any], suite: str = "S") -> dict[str, Any]:
    try:
        durable = suites.get(suite).durable
    except KeyError as e:
        raise ConfigError(str(e.args[0])) from None
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
        if not durable:
            break
        actual = cfg["postgres"].get(k, v)
        if k == "synchronous_commit" and cfg["run"]["comparison_only"] and actual == "off":
            continue
        if actual != v:
            raise ConfigError(f"{k} = {actual}: the procedure requires {k} = {v} "
                              "(results with it off are not reported)")
        cfg["postgres"][k] = v
    return cfg


_GUC_SECONDS = {"us": 1e-6, "ms": 1e-3, "s": 1, "min": 60, "h": 3600, "d": 86400}


def guc_seconds(value) -> float:
    """A time setting ("5min", "1d", "300") in seconds; bare numbers are seconds."""
    m = re.fullmatch(r"\s*([0-9.]+)\s*([a-z]*)\s*", str(value))
    if not m or m.group(2) not in _GUC_SECONDS and m.group(2):
        raise ConfigError(f"unreadable time setting {value!r}")
    return float(m.group(1)) * _GUC_SECONDS.get(m.group(2) or "s", 1)


def guc_bytes(value) -> float:
    """A WAL size setting in bytes; bare numbers use PostgreSQL's MB unit."""
    units = {"B": 1, "kB": 1 << 10, "MB": 1 << 20, "GB": 1 << 30, "TB": 1 << 40}
    match = re.fullmatch(r"\s*([0-9]+(?:\.[0-9]+)?)\s*(B|kB|MB|GB|TB)?\s*", str(value))
    if not match:
        raise ConfigError(f"unreadable WAL size setting {value!r}")
    return float(match.group(1)) * units[match.group(2) or "MB"]


def validate(cfg: dict, suite: str) -> None:
    run = cfg["run"]
    correctness = suites.layout(suite) == suites.CORRECTNESS
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
        number(run[name], name, 0 if name != "measure_seconds" or correctness else 1)
        if type(run[name]) is not int:
            raise ConfigError(f"{name} must be an integer")
    number(run["latency_sampling_rate"], "latency_sampling_rate", 1e-12, 1)
    if run["shard"]:
        if not correctness:
            raise ConfigError("only a correctness suite (C) runs in shards")
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
    for name in ("settled_counters", "comparison_only"):
        if type(run[name]) is not bool:
            raise ConfigError(f"{name} must be true or false")
    if run["settled_counters"] and suite == "S" and (not run["workloads"] or "S6" in run["workloads"]):
        raise ConfigError("settled_counters waits until no other client is connected, and S6 "
                          "holds a session open for its whole window: leave S6 out")
    if run["write_amp_materialize"] not in ("auto", "prolly_checkpoint", "none"):
        raise ConfigError("write_amp_materialize must be auto, prolly_checkpoint or none")
    pg = cfg.get("postgres", {})
    if ("write_amp" in run["collectors"] and pg.get("wal_level", "replica") != "minimal"
            and str(pg.get("max_replication_slots", "10")) == "0"):
        raise ConfigError("write_amp holds a replication slot over its interval, so the WAL "
                          "it splits isn't recycled: give it max_replication_slots >= 1")
    if "write_amp" in run["collectors"] and pg.get("wal_level") == "minimal":
        # No slot: timed, volume-triggered and workload checkpoints can
        # recycle the WAL before the collector reads it. S4 checkpoints
        # explicitly inside its measurement window.
        timeout = guc_seconds(pg.get("checkpoint_timeout", "5min"))
        longest = max([run["measure_seconds"]] + [
            p["measure_seconds"] for p in cfg.get("workloads", {}).values()
            if "measure_seconds" in p])
        if timeout <= longest:
            raise ConfigError(f"write_amp at wal_level = minimal keeps no replication slot: "
                              f"checkpoint_timeout ({timeout:.0f} s) must exceed the measured "
                              f"window ({longest} s)")
        if (suite == "S" and (not run["workloads"] or "S4" in run["workloads"])
                and guc_bytes(pg.get("wal_keep_size", "0")) < (16 << 20)):
            raise ConfigError("S4 checkpoints inside its write_amp interval at wal_level = minimal: "
                              "set wal_keep_size to retain the interval's WAL (at least one 16MB segment), "
                              "or use wal_level = replica with a replication slot")
    if "write_amp" in run["collectors"] and "recovery" in run["collectors"]:
        raise ConfigError("write_amp checkpoints after the window, which leaves the recovery "
                          "collector nothing to replay: run them in separate configs")
    if not set(run["required_collectors"]) <= set(run["collectors"]):
        raise ConfigError("required_collectors must be enabled")
    for label, sizes in cfg["sizes"].items():
        for name, value in sizes.items():
            if name not in suites.size_keys():
                raise ConfigError(f"unknown size parameter {label}.{name}")
            number(value, f"sizes.{label}.{name}", 1e-12)
            if name != "tpch_scale" and type(value) is not int:
                raise ConfigError(f"sizes.{label}.{name} must be an integer")
    common = {"clients", "warmup_seconds", "measure_seconds", "protocol",
              "perf_frequency", "hash_symbols", "latency_sampling_rate", "timeout_seconds",
              "rate", "latency_limit_ms", "transactions"}
    keys = {"S3": {"ranges"}, "S5": {"hot_rows"}, "S8": {"percents"},
            "S9": {"isolation", "weights"}, "S10": {"tests", "dolt_options"}, "S13": {"modes", "weights", "isolation"},
            "V1": {"iterations", "background", "history_window"},
            "V2": {"iterations", "background", "changes", "history_window"},
            "V3": {"background", "changes", "locality", "conflict_percent", "history_window"},
            "V4": {"background", "changes", "history_window"},
            "V6": {"background", "branches", "changes", "history_window"}, "V7": {"background", "commit_modes", "commits"},
            "V8": {"background", "commits", "update_percent"},
            "V9": {"background", "rows", "delta", "delta_rows", "deltas", "others",
                   "others_rows", "branches"},
            "V10": {"background", "caches", "reps"},
            "V11": {"background", "churn", "warm_cycles", "cycles", "sweep", "sweep_cycles",
                    "window_cycles", "reads", "standby", "retire", "conjoin_files", "full",
                    "verify", "load_batch"}}
    keys.update({f"C{i}": {"workers", "sample"} for i in range(1, 5)})
    keys.update({f"S{i}": set() for i in (1, 2, 4, 6, 7)})
    # Workloads of the suites that register their parameters.
    keys.update(suites.workload_params())
    for wid, params in cfg["workloads"].items():
        if wid not in keys or set(params) - (keys[wid] | common):
            raise ConfigError(f"unknown workload or parameters for {wid}: {sorted(params)}")
        if wid == "S10" and "tests" in params:
            from pbench.workloads.sysbench import DEFAULT_TESTS, DOLT_TESTS
            tests = params["tests"]
            if tests != "dolt" and (type(tests) is not list or
                                    set(tests) - set(DEFAULT_TESTS) - set(DOLT_TESTS)):
                raise ConfigError(f'S10.tests must be "dolt" or a list of sysbench tests: {tests!r}')
        if wid == "S10" and type(params.get("dolt_options", False)) is not bool:
            raise ConfigError("S10.dolt_options must be true or false")
        for name, choices in {"sample": {"all", "per_directory"},
                              "protocol": {"simple", "extended", "prepared"}}.items():
            if name in params and params[name] not in choices:
                raise ConfigError(f"invalid {wid}.{name}")
        for name, choices in {"modes": {"serial", "throughput"},
                              # (S1 behind V's workloads, G5's moving
                              # objects behind GV's)
                              "background": {"none", "G5"} if wid.startswith("GV") else {"none", "S1"},
                              "locality": {"clustered", "scattered"},
                              "overlap": {"disjoint", "overlapping"},
                              "predicates": {"bbox", "intersects"}}.items():
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
        if "transactions" in params and wid not in suites.transaction_workloads():
            raise ConfigError("transactions is supported only by pgbench and sysbench workloads")
        for name in ("rate", "latency_limit_ms"):
            if name in params:
                if wid not in suites.rate_workloads():
                    raise ConfigError(f"{name} is supported only by pgbench workloads")
                number(params[name], f"{wid}.{name}", 1e-12)


def server_settings(cfg: dict[str, Any]) -> dict[str, str]:
    return {**HARNESS_SETTINGS, **cfg["postgres"]}
