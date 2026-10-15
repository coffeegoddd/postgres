"""The S suite: storage workloads that run on any table AM.  Every table is
created with the table AM under test through default_table_access_method."""

from __future__ import annotations

import subprocess
import time
from typing import Any

from pbench.core import registry
from pbench.core.interfaces import Context, Outcome, Window, Workload
from pbench.workloads._common import (PgbenchResult, check_client_error,
                                      pgbench_init, run_pgbench)


def _outcome(res: PgbenchResult, read_only: bool = False) -> Outcome:
    return Outcome(metrics=res.metrics(), transactions=res.transactions,
                   client_cpu_seconds=res.client_cpu_seconds, read_only=read_only)


class PgbenchWorkload(Workload):
    """A workload that is one pgbench run over the pgbench tables."""

    builtin: str | None = None
    scripts: list[tuple[str, int]] = []
    read_only = False

    def scale(self, ctx: Context) -> int:
        return int(ctx.size_params["pgbench_scale"])

    def defines(self, ctx: Context) -> dict[str, Any]:
        return {"scale": self.scale(ctx)}

    def prepare(self, ctx: Context) -> None:
        pgbench_init(ctx, self.scale(ctx))

    def _run(self, ctx: Context, seconds: int, tag: str, **kw) -> PgbenchResult:
        return run_pgbench(ctx, seconds=seconds, builtin=self.builtin,
                           scripts=self.scripts, defines=self.defines(ctx),
                           tag=tag, **kw)

    def warmup(self, ctx: Context) -> None:
        if ctx.warmup_seconds > 0:
            self._run(ctx, ctx.warmup_seconds, "warmup", log_latency=False)

    def measure(self, ctx: Context, window: Window) -> Outcome:
        with window:
            res = self._run(ctx, ctx.measure_seconds, "measure")
        return _outcome(res, self.read_only)

    def tables(self, ctx: Context) -> list[str]:
        return ["pgbench_accounts", "pgbench_branches", "pgbench_tellers",
                "pgbench_history"]


@registry.workloads.register("S1")
class S1(PgbenchWorkload):
    id, suite, title = "S1", "S", "TPC-B-like read/write (pgbench default)"
    builtin = "tpcb-like"


@registry.workloads.register("S2")
class S2(PgbenchWorkload):
    id, suite, title = "S2", "S", "Read-only point lookups (pgbench -S)"
    builtin = "select-only"
    read_only = True


@registry.workloads.register("S3")
class S3(PgbenchWorkload):
    id, suite, title = "S3", "S", "Range scans"
    scripts = [("s3_range.sql", 1)]
    read_only = True

    def variants(self, params):
        return [(f"range{r}", {"range": int(r)}) for r in params.get("ranges", [100])]

    def defines(self, ctx):
        return {"scale": self.scale(ctx), "range": ctx.params["range"]}


@registry.workloads.register("S4")
class S4(Workload):
    """COPY a large table in, then build indexes and checkpoint."""

    id, suite, title = "S4", "S", "Bulk load (COPY, then index build)"

    def client_counts(self, configured, variant):
        return [1]

    def _file(self, ctx: Context):
        return ctx.cluster.data_dir.parent / "s4.copy"

    def prepare(self, ctx: Context) -> None:
        rows = int(ctx.size_params["s4_rows"])
        # The source rows are generated in a heap table and written to a file,
        # so generation is not part of the measurement.  The heap table is in
        # public: a versioned schema holds only prolly tables.
        ctx.cluster.sql(f"""
            CREATE TABLE public.s4_src USING heap AS
              SELECT g::bigint AS id, (g * 7919 % {rows})::int AS k,
                     md5(g::text) || md5((g + 1)::text) AS payload
              FROM generate_series(1, {rows}) g;
            COPY public.s4_src TO '{self._file(ctx)}';
            DROP TABLE public.s4_src;
            CREATE TABLE s4 (id bigint, k int, payload text);
        """, dbname=ctx.dbname, options=ctx.session_options())

    def measure(self, ctx: Context, window: Window) -> Outcome:
        rows = int(ctx.size_params["s4_rows"])
        timings: dict[str, float] = {}
        steps = [("copy_seconds", f"COPY s4 FROM '{self._file(ctx)}'"),
                 ("index_seconds", "CREATE INDEX s4_id ON s4 (id); "
                                   "CREATE INDEX s4_k ON s4 (k)"),
                 ("checkpoint_seconds", "CHECKPOINT")]
        with window:
            for name, sql in steps:
                t0 = time.monotonic()
                ctx.cluster.sql(sql, dbname=ctx.dbname, options=ctx.session_options())
                timings[name] = time.monotonic() - t0
        self._file(ctx).unlink(missing_ok=True)
        total = sum(timings.values())
        return Outcome(metrics={**timings, "total_seconds": total,
                                "rows": rows,
                                "rows_per_second": rows / timings["copy_seconds"]},
                       transactions=rows, unit="row")

    def tables(self, ctx):
        return ["s4"]


@registry.workloads.register("S5")
class S5(PgbenchWorkload):
    id, suite, title = "S5", "S", "Hot-row contention"
    scripts = [("s5_hot.sql", 1)]

    def prepare(self, ctx):
        n = int(ctx.params.get("hot_rows", 10))
        ctx.cluster.sql(f"""
            CREATE TABLE s5_hot (id int PRIMARY KEY, v bigint NOT NULL);
            INSERT INTO s5_hot SELECT g, 0 FROM generate_series(1, {n}) g;
        """, dbname=ctx.dbname, options=ctx.session_options())

    def defines(self, ctx):
        return {"hot_rows": int(ctx.params.get("hot_rows", 10))}

    def tables(self, ctx):
        return ["s5_hot"]


@registry.workloads.register("S6")
class S6(PgbenchWorkload):
    """S1 while one REPEATABLE READ transaction stays open the whole time."""

    id, suite, title = "S6", "S", "Long snapshot under writes"
    builtin = "tpcb-like"

    def measure(self, ctx, window):
        holder = subprocess.Popen(
            [ctx.cluster.bin("psql"), "-X", "-q", "-A", "-t", "-d", ctx.dbname],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, env=ctx.cluster.client_env(ctx.session_options()))
        assert holder.stdin and holder.stdout
        holder.stdin.write("BEGIN ISOLATION LEVEL REPEATABLE READ;\n"
                           "SELECT count(*) FROM pgbench_branches;\n"
                           "\\echo SNAPSHOT_HELD\n")
        holder.stdin.flush()
        seen = []
        for line in holder.stdout:
            seen.append(line)
            if "SNAPSHOT_HELD" in line:
                break
        else:
            out = "".join(seen)
            check_client_error(out)
            raise RuntimeError(f"snapshot holder failed: {out}")
        try:
            with window:
                res = self._run(ctx, ctx.measure_seconds, "measure")
        finally:
            holder.stdin.write("COMMIT;\n\\q\n")
            holder.stdin.close()
            holder.wait(timeout=60)
        out = _outcome(res)
        out.notes.append("one REPEATABLE READ snapshot held for the whole window")
        return out


@registry.workloads.register("S7")
class S7(PgbenchWorkload):
    id, suite, title = "S7", "S", "Secondary-index heavy (4 btree + 1 GiST)"
    scripts = [("s7_update.sql", 70), ("s7_insert.sql", 20), ("s7_select.sql", 10)]

    def rows(self, ctx):
        return int(ctx.size_params["s7_rows"])

    def prepare(self, ctx):
        n = self.rows(ctx)
        ctx.cluster.sql(f"""
            CREATE TABLE s7 (id bigint PRIMARY KEY, a int, b int, c int,
                             d text, p point);
            INSERT INTO s7
              SELECT g, g % 1000, g % 100000, g % 7919, md5(g::text),
                     point(g % 1000, g % 997)
              FROM generate_series(1, {n}) g;
            CREATE INDEX s7_a ON s7 (a);
            CREATE INDEX s7_b ON s7 (b);
            CREATE INDEX s7_c ON s7 (c);
            CREATE INDEX s7_d ON s7 (d);
            CREATE INDEX s7_p ON s7 USING gist (p);
            VACUUM ANALYZE s7;
        """, dbname=ctx.dbname, options=ctx.session_options())

    def defines(self, ctx):
        return {"rows": self.rows(ctx)}

    def tables(self, ctx):
        return ["s7"]


@registry.workloads.register("S8")
class S8(PgbenchWorkload):
    id, suite, title = "S8", "S", "Large transactions"
    scripts = [("s8_large.sql", 1)]

    def variants(self, params):
        return [(f"pct{p}", {"percent": float(p)})
                for p in params.get("percents", [1, 10])]

    def defines(self, ctx):
        rows = 100000 * self.scale(ctx)
        return {"scale": self.scale(ctx),
                "n": max(1, int(rows * ctx.params["percent"] / 100))}


@registry.workloads.register("S9")
class S9(Workload):
    """BenchBase TPC-C (_benchbase.py does the BenchBase plumbing)."""

    id, suite, title = "S9", "S", "Mixed OLTP (BenchBase TPC-C)"

    def check(self, ctx):
        from pbench.workloads._benchbase import check
        check(ctx)

    def prepare(self, ctx):
        from pbench.workloads._benchbase import load
        load(ctx, "tpcc", scalefactor=int(ctx.size_params["tpcc_warehouses"]))

    def warmup(self, ctx):
        from pbench.workloads._benchbase import execute
        if ctx.warmup_seconds > 0:
            execute(ctx, "tpcc", None, seconds=ctx.warmup_seconds,
                    scalefactor=int(ctx.size_params["tpcc_warehouses"]))

    def measure(self, ctx, window):
        from pbench.workloads._benchbase import execute
        return execute(ctx, "tpcc", window,
                       scalefactor=int(ctx.size_params["tpcc_warehouses"]))

    def tables(self, ctx):
        return ["warehouse", "district", "customer", "history", "oorder",
                "new_order", "order_line", "item", "stock"]
