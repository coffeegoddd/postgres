"""S10: sysbench OLTP tests (https://github.com/akopytov/sysbench).

Each sysbench test is a variant.  The tests are the Lua scripts shipped with
sysbench; sysbench is also what Dolt and Doltgres publish their latency
comparisons with, so these numbers can be set beside theirs.

Tables are created by `sysbench prepare` through libpq, which picks up
PGOPTIONS, so they use the table AM under test like every other workload.

Dolt's benchmarks also run scripts of their own (scans, group-by, joins, a
many-typed table: https://github.com/dolthub/sysbench-lua-scripts).  With
[tools] sysbench_lua_scripts naming a checkout of them, those tests can be
run too, by name, from its *_postgres.lua scripts; `tests = "dolt"` is
every test Dolt's benchmarks run.  The scripts make their own tables, of
`table_size` rows (oltp_delete_insert: 10,000 always), and take no
--tables.  `dolt_options = true` runs sysbench as Dolt's benchmarks do
(DOLT_OPTIONS), with no prepared statements and uniformly random keys.
"""

from __future__ import annotations

from pathlib import Path
import re
import shutil
from pbench.core import process
from typing import Optional

from pbench.core import registry
from pbench.core.interfaces import Context, Outcome, Unavailable, Window, Workload
from pbench.core.stats import LatencyHistogram
from pbench.workloads._common import (check_client_error, children_cpu_seconds,
                                      save_histogram)

DEFAULT_TESTS = [
    "oltp_read_write", "oltp_read_only", "oltp_write_only", "oltp_point_select",
    "oltp_update_index", "oltp_update_non_index", "oltp_insert",
    "select_random_ranges", "bulk_insert",
]

# Dolt's own scripts, in the sysbench_lua_scripts checkout as
# <name>_postgres.lua, and the tables each makes.
DOLT_SCRIPTS = {
    "covering_index_scan": ["sbtest1", "sbtest2"], "groupby_scan": ["sbtest1", "sbtest2"],
    "index_join": ["sbtest1", "sbtest2"], "index_join_scan": ["sbtest1", "sbtest2"],
    "index_scan": ["sbtest1", "sbtest2"], "table_scan": ["sbtest1", "sbtest2"],
    "types_table_scan": ["sbtest1"], "oltp_delete_insert": ["sbtest1"],
    "types_delete_insert": ["sbtest1"],
}

# The tests Dolt's benchmarks run: sysbench's own and its scripts.
DOLT_TESTS = [
    "oltp_read_only", "oltp_insert", "bulk_insert", "oltp_point_select",
    "select_random_points", "select_random_ranges", "oltp_write_only",
    "oltp_read_write", "oltp_update_index", "oltp_update_non_index",
    *DOLT_SCRIPTS,
]

# The options Dolt's benchmarks run sysbench with.
DOLT_OPTIONS = ["--db-ps-mode=disable", "--rand-type=uniform"]

# Tests that only read.
_READ_ONLY = {"oltp_read_only", "oltp_point_select", "select_random_points",
              "select_random_ranges", "covering_index_scan", "groupby_scan",
              "index_join", "index_join_scan", "index_scan", "table_scan",
              "types_table_scan"}

# Tests whose scripts only support a single table.
_ONE_TABLE = {"select_random_points", "select_random_ranges"}


def parse_sysbench_output(text: str, hist: LatencyHistogram) -> dict[str, Optional[float]]:
    """Totals from a `sysbench run` report; latencies from its --histogram
    (milliseconds) go into hist."""
    def num(pattern: str) -> Optional[float]:
        m = re.search(pattern, text)
        return float(m.group(1)) if m else None

    for m in re.finditer(r"^\s*([\d.]+)\s+\|\**\s+(\d+)\s*$", text, re.M):
        hist.add(float(m.group(1)) * 1000.0, int(m.group(2)))
    return {
        "transactions": num(r"transactions:\s+(\d+)"),
        "tps": num(r"transactions:\s+\d+\s+\(([\d.]+) per sec"),
        "queries": num(r"queries:\s+(\d+)"),
        "qps": num(r"queries:\s+\d+\s+\(([\d.]+) per sec"),
        "events": num(r"total number of events:\s+(\d+)"),
        "ignored_errors": num(r"ignored errors:\s+(\d+)"),
        "sysbench_p99_ms": num(r"99th percentile:\s+([\d.]+)"),
    }


@registry.workloads.register("S10")
class S10(Workload):
    id, suite, title = "S10", "S", "sysbench OLTP"

    def variants(self, params):
        tests = params.get("tests", DEFAULT_TESTS)
        return [(t, {"test": t}) for t in (DOLT_TESTS if tests == "dolt" else tests)]

    def check(self, ctx: Context) -> None:
        if shutil.which("sysbench") is None:
            raise Unavailable("sysbench is not installed")
        if ctx.params["test"] in DOLT_SCRIPTS and not self._script(ctx).is_file():
            raise Unavailable(f"no {self._script(ctx).name} under [tools] sysbench_lua_scripts = "
                              f"{ctx.tools.get('sysbench_lua_scripts')}")

    def _script(self, ctx: Context) -> Path:
        return Path(ctx.tools.get("sysbench_lua_scripts") or "") / f"{ctx.params['test']}_postgres.lua"

    def _tables(self, ctx: Context) -> int:
        test = ctx.params["test"]
        if test in DOLT_SCRIPTS:
            return len(DOLT_SCRIPTS[test])
        if test in _ONE_TABLE:
            return 1
        if test == "bulk_insert":
            return ctx.key.clients   # bulk_insert uses one table per thread
        return int(ctx.size_params.get("sysbench_tables", 10))

    def _args(self, ctx: Context, command: str, extra: list[str]) -> list[str]:
        env = ctx.cluster.client_env()
        script = ctx.params["test"] in DOLT_SCRIPTS
        return ["sysbench", str(self._script(ctx)) if script else ctx.params["test"], "--db-driver=pgsql",
                f"--pgsql-host={env['PGHOST']}", f"--pgsql-port={env['PGPORT']}",
                "--pgsql-user=postgres", f"--pgsql-db={ctx.dbname}",
                *([] if script else [f"--tables={self._tables(ctx)}"]),
                f"--table-size={int(ctx.size_params.get('sysbench_table_size', 100000))}",
                f"--threads={ctx.key.clients}", f"--rand-seed={ctx.params.get('seed', 1)}",
                *(DOLT_OPTIONS if ctx.params.get("dolt_options") else []), *extra, command]

    def _sysbench(self, ctx: Context, command: str, tag: str,
                  extra: Optional[list[str]] = None) -> str:
        args = self._args(ctx, command, extra or [])
        env = ctx.cluster.client_env(ctx.session_options())
        if ctx.params["test"] in DOLT_SCRIPTS:
            # (the scripts require their common code by module name; ";;"
            # keeps sysbench's own after it)
            env = {**env, "LUA_PATH": str(self._script(ctx).parent / "?.lua") + ";;"}
        r = process.run(args, capture_output=True, text=True,
                           timeout=int(ctx.params.get("timeout_seconds", 7200)),
                           env=env)
        out = " ".join(args) + "\n\n" + r.stdout + r.stderr
        (ctx.raw_dir / f"sysbench-{tag}.out").write_text(out)
        if r.returncode != 0:
            check_client_error(out)
            raise RuntimeError(f"sysbench {command} failed (exit {r.returncode}): "
                               f"{out[-2000:]}")
        return out

    def prepare(self, ctx: Context) -> None:
        self._sysbench(ctx, "prepare", "prepare")
        ctx.cluster.sql("VACUUM ANALYZE", dbname=ctx.dbname,
                        options=ctx.session_options())

    def warmup(self, ctx: Context) -> None:
        if ctx.warmup_seconds > 0:
            self._sysbench(ctx, "run", "warmup", [f"--time={ctx.warmup_seconds}"])
            if ctx.params["test"] == "bulk_insert":
                # bulk_insert numbers rows from 1 on every run, so the
                # measured run needs the empty tables back.
                self._sysbench(ctx, "cleanup", "warmup-cleanup")
                self._sysbench(ctx, "prepare", "warmup-prepare")
                ctx.cluster.sql("CHECKPOINT")

    def measure(self, ctx: Context, window: Window) -> Outcome:
        # (a fixed amount of work: that many events, in all, however long)
        events = ctx.params.get("transactions")
        amount = [f"--events={events}", "--time=0"] if events else \
            [f"--time={ctx.measure_seconds}"]
        with window:
            cpu0 = children_cpu_seconds()
            out = self._sysbench(ctx, "run", "measure",
                                 [*amount, "--histogram=on", "--percentile=99"])
            client_cpu = children_cpu_seconds() - cpu0
        hist = LatencyHistogram()
        parsed = parse_sysbench_output(out, hist)
        if events and parsed["events"] != events:
            raise RuntimeError(f"sysbench ran {parsed['events']} events, not the {events} asked for")
        save_histogram(ctx, "measure", hist)

        def ms(us):
            return None if us is None else us / 1000.0

        if ctx.params["test"] == "bulk_insert":
            # Each bulk_insert event is one row added to a multi-row INSERT,
            # so per-event latency means nothing; report row throughput.
            rows = parsed["events"]
            return Outcome(metrics={"rows": rows,
                                    "rows_per_second": rows / window.seconds
                                    if rows and window.seconds else None},
                           transactions=rows, client_cpu_seconds=client_cpu,
                           unit="row")
        metrics = {**parsed,
                   "latency_samples": hist.count,
                   "latency_mean_ms": ms(hist.mean()),
                   "latency_p50_ms": ms(hist.reported_percentile(50)),
                   "latency_p99_ms": ms(hist.reported_percentile(99)),
                   "latency_p999_ms": ms(hist.reported_percentile(99.9)),
                   "latency_max_ms": ms(hist.max_us or None)}
        return Outcome(metrics=metrics, transactions=parsed["transactions"],
                       client_cpu_seconds=client_cpu,
                       read_only=ctx.params["test"] in _READ_ONLY)

    def tables(self, ctx: Context) -> list[str]:
        if ctx.params["test"] in DOLT_SCRIPTS:
            return list(DOLT_SCRIPTS[ctx.params["test"]])
        return [f"sbtest{i}" for i in range(1, self._tables(ctx) + 1)]
