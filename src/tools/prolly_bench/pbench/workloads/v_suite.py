"""The V suite: version-control workloads, for prolly, driven through the
dolt_* SQL functions (its tables are in a versioned schema, committed with
dolt_commit).

Every V workload has a "background" variant dimension: "none", or "S1" to
run pgbench TPC-B alongside the version-control operations, measuring how
each affects the other.  With background S1 the client count is the number
of background pgbench clients; without it, only one client count is run.
"""

from __future__ import annotations

import statistics
import math
from contextlib import nullcontext
from itertools import product
from typing import Any

from pbench.core import registry, targets
from pbench.core.interfaces import Context, NotImplementedByEngine, Outcome, Window, Workload
from pbench.workloads._common import BackgroundPgbench, pgbench_init, timed_session


def _median(xs: list[float]) -> float | None:
    return statistics.median(xs) if xs else None


class _DoltSQL:
    """Version-control statements of page-based prolly."""

    def branch(self, name):
        return f"SELECT dolt_branch('{name}')"

    def checkout(self, name):
        return f"SELECT dolt_checkout('{name}')"

    def commit(self, message):
        return f"SELECT dolt_commit('-Am', '{message}')"

    def diff_count(self, a, b, table):
        return f"SELECT count(*) FROM dolt_diff('{a}', '{b}', NULL::{table})"

    def merge(self, name):
        return f"SELECT dolt_merge('{name}')"


# The version-control statements every V workload uses.
VC_SQL = _DoltSQL()


class VWorkload(Workload):
    suite = "V"

    def check(self, ctx):
        if ctx.table_am not in targets.VERSIONED_HISTORY:
            raise NotImplementedByEngine(f"{ctx.table_am}: no verified data-history adapter for V benchmarks")
        # Same data-history requirement as vcscenario: restoring a committed
        # value must actually restore SQL-visible data. Timing a ref-only
        # function is not a version-control benchmark.
        db = "pbench_vc_probe"
        ctx.cluster.sql(f"CREATE DATABASE {db}")
        try:
            setup = targets.for_am(ctx.table_am).setup_sql(db)
            if setup:
                ctx.cluster.sql(setup, dbname=db)
            ctx.cluster.sql(f"""
                CREATE TABLE target (id int PRIMARY KEY, v text);
                INSERT INTO target VALUES (1, 'base');
                {VC_SQL.commit('probe base')};
                UPDATE target SET v = 'dirty';
                SELECT dolt_reset('--hard', 'HEAD');
                DO $$ BEGIN
                  IF (SELECT v FROM target WHERE id=1) IS DISTINCT FROM 'base' THEN
                    RAISE EXCEPTION 'history does not restore SQL data';
                  END IF;
                END $$;
            """, dbname=db, options=ctx.session_options())
        finally:
            ctx.cluster.sql(f"DROP DATABASE {db} WITH (FORCE)")

    def own_variants(self, params: dict[str, Any]) -> list[tuple[str, dict[str, Any]]]:
        return [("", {})]

    def variants(self, params):
        out = []
        for (label, extra), bg in product(self.own_variants(params),
                                          params.get("background", ["none", "S1"])):
            name = "-".join(x for x in (label, f"bg{bg}") if x)
            out.append((name, {**extra, "background": bg}))
        return out

    def client_counts(self, configured, variant):
        return configured if variant["background"] != "none" else configured[:1]

    def rows(self, ctx: Context) -> int:
        return int(ctx.size_params.get("v_rows", 100000))

    def prepare(self, ctx: Context) -> None:
        n = self.rows(ctx)
        ctx.cluster.sql(f"""
            CREATE TABLE vc_data (id bigint PRIMARY KEY, payload text NOT NULL);
            INSERT INTO vc_data SELECT g, md5(g::text) FROM generate_series(1, {n}) g;
        """, dbname=ctx.dbname, options=ctx.session_options())
        if ctx.params["background"] == "S1":
            pgbench_init(ctx, int(ctx.size_params["pgbench_scale"]))
        timed_session(ctx, [VC_SQL.commit("initial data")], "prepare-commit")

    def background(self, ctx: Context):
        if ctx.params["background"] == "S1":
            return BackgroundPgbench(ctx, "tpcb-like", ctx.key.clients, "background")
        if ctx.params["background"] != "none":
            raise ValueError(f"unknown background workload {ctx.params['background']}")
        return nullcontext()

    def measure(self, ctx: Context, window: Window) -> Outcome:
        with self.background(ctx) as bg:
            with window:
                outcome = self.operations(ctx)
        if bg is not None:
            outcome.metrics["background_tps"] = bg.tps()
        return outcome

    def operations(self, ctx: Context) -> Outcome:
        raise NotImplementedError

    def tables(self, ctx):
        return ["vc_data"] + (["pgbench_accounts", "pgbench_branches", "pgbench_tellers",
                               "pgbench_history"] if ctx.params["background"] == "S1" else [])

    def iterations(self, ctx: Context) -> int:
        return int(ctx.params.get("iterations", 10))


@registry.workloads.register("V1")
class V1(VWorkload):
    id, title = "V1", "Branch create and checkout"

    def operations(self, ctx):
        vc = VC_SQL
        stmts = []
        for i in range(self.iterations(ctx)):
            stmts += [vc.branch(f"v1_b{i}"), vc.checkout(f"v1_b{i}"),
                      vc.checkout("main")]
        t = timed_session(ctx, stmts, "v1")
        create, checkout = t[0::3], t[1::3] + t[2::3]
        return Outcome(metrics={"branch_create_ms": _median(create),
                                "checkout_ms": _median(checkout)},
                       transactions=len(t), unit="operation")


@registry.workloads.register("V2")
class V2(VWorkload):
    id, title = "V2", "Commit after N row changes"

    def own_variants(self, params):
        return [(f"n{n}", {"changes": int(n)})
                for n in params.get("changes", [1, 100, 10000])]

    def prepare(self, ctx):
        super().prepare(ctx)
        # on a branch of its own, so a background workload's writes (on
        # main) aren't in its commits; made before the measured window
        timed_session(ctx, [VC_SQL.branch("v2_work")], "v2-prepare")

    def operations(self, ctx):
        n = ctx.params["changes"]
        if n > self.rows(ctx):
            raise ValueError("V2 changes exceed the dataset; refusing to clamp the workload")
        vc = VC_SQL
        # (a checkout is the session's own, so it is this session's first
        # statement; the timings leave it out)
        stmts = [vc.checkout("v2_work")]
        for i in range(self.iterations(ctx)):
            stmts += [f"UPDATE vc_data SET payload = md5(payload || '{i}') "
                      f"WHERE id <= {n}",
                      vc.commit(f"v2 {i}")]
        t = timed_session(ctx, stmts, "v2")[1:]
        return Outcome(metrics={"change_ms": _median(t[0::2]),
                                "commit_ms": _median(t[1::2])},
                       transactions=len(t) // 2, unit="commit")


@registry.workloads.register("V3")
class V3(VWorkload):
    """Diff and three-way merge of two branches with controlled change sets:
    size, locality (clustered or scattered ids) and conflicting rows."""

    id, title = "V3", "Diff and merge"

    def own_variants(self, params):
        out = []
        for n, loc, conf in product(params.get("changes", [100, 10000]),
                                    params.get("locality", ["clustered", "scattered"]),
                                    params.get("conflict_percent", [0])):
            out.append((f"n{n}-{loc}-conf{conf}",
                        {"changes": int(n), "locality": loc, "conflict_percent": conf}))
        return out

    def _ids(self, ctx, side: int) -> str:
        """SQL predicate selecting this side's changed rows."""
        n, rows = ctx.params["changes"], self.rows(ctx)
        overlap = int(n * ctx.params["conflict_percent"] / 100)
        if n <= 0 or 2 * n - overlap > rows:
            raise ValueError("V3 dataset cannot hold the requested change sets and overlap")
        lo = side * (n - overlap)
        if ctx.params["locality"] == "clustered":
            return f"id BETWEEN {lo + 1} AND {lo + n}"
        stride = max(1, rows // n)
        while math.gcd(stride, rows) != 1:
            stride += 1
        # A bijection of [0, rows) gives exactly n rows per side and exactly
        # overlap shared rows, including dense changes where old stride=1.
        return f"((id - 1) * {stride} % {rows}) BETWEEN {lo} AND {lo + n - 1}"

    def operations(self, ctx):
        predicates = [self._ids(ctx, i) for i in (0, 1)]
        n = ctx.params["changes"]
        overlap = int(n * ctx.params["conflict_percent"] / 100)
        observed = ctx.cluster.scalar(
            f"SELECT count(*) FILTER (WHERE {predicates[0]}), "
            f"count(*) FILTER (WHERE {predicates[1]}), "
            f"count(*) FILTER (WHERE ({predicates[0]}) AND ({predicates[1]})) FROM vc_data",
            dbname=ctx.dbname)
        if observed != f"{n}|{n}|{overlap}":
            raise RuntimeError(f"V3 change-set cardinalities differ: {observed}")
        # Both sides are branches of their own, so a background workload's
        # writes (on main) are on neither and the merge doesn't touch them.
        vc = VC_SQL
        stmts = [
            vc.branch("v3_ours"),
            vc.branch("v3_other"),
            vc.checkout("v3_other"),
            f"UPDATE vc_data SET payload = 'other' WHERE {self._ids(ctx, 1)}",
            vc.commit("v3 other side"),
            vc.checkout("v3_ours"),
            f"UPDATE vc_data SET payload = 'ours' WHERE {self._ids(ctx, 0)}",
            vc.commit("v3 our side"),
            vc.diff_count("v3_ours", "v3_other", "vc_data"),
            vc.merge("v3_other"),
        ]
        t = timed_session(ctx, stmts, "v3")
        return Outcome(metrics={"diff_ms": t[8], "merge_ms": t[9]},
                       transactions=2, unit="operation")


@registry.workloads.register("V4")
class V4(VWorkload):
    """Clone, then fetch and push, between this instance and a second local
    instance, over a network remote (postgresql://): the second clones the
    data, fetches a commit made since, and pushes one back (to a branch of
    its own, so a background workload's writes on main don't refuse it)."""

    id, title = "V4", "Clone, fetch and push between two instances"

    def own_variants(self, params):
        return [(f"n{n}", {"changes": int(n)})
                for n in params.get("changes", [1000])]

    def measure(self, ctx, window):
        remote = ctx.make_cluster("v4-remote")
        try:
            remote.sql(f"CREATE DATABASE {ctx.dbname}")
            # (the clone makes the versioned schema; its search path is the
            # origin's, as the target's setup sets it)
            schema = targets.for_am(ctx.table_am).schema
            if schema:
                remote.sql(f"ALTER DATABASE {ctx.dbname} SET search_path = {schema}, public")
            with self.background(ctx) as bg:
                with window:
                    outcome = self.operations(ctx, remote)
            if bg is not None:
                outcome.metrics["background_tps"] = bg.tps()
            return outcome
        finally:
            remote.destroy()

    def operations(self, ctx, remote=None):
        n = ctx.params["changes"]
        if n > self.rows(ctx):
            raise ValueError("V4 changes exceed the dataset; refusing to clamp the workload")
        url = ctx.cluster.libpq_url(ctx.dbname)
        clone = timed_session(ctx, [f"SELECT dolt_clone('{url}')"], "v4-clone",
                              cluster=remote)
        # a commit at the origin, on a branch of its own, for the fetch
        timed_session(ctx, [VC_SQL.branch("v4_origin"), VC_SQL.checkout("v4_origin"),
                            f"UPDATE vc_data SET payload = md5(payload) WHERE id <= {n}",
                            VC_SQL.commit("v4 origin change")], "v4-origin")
        fetch = timed_session(ctx, ["SELECT dolt_fetch('origin')"], "v4-fetch",
                              cluster=remote)
        # a commit at the clone, pushed back as a new branch
        push = timed_session(ctx, [
            "SELECT dolt_checkout('-b', 'v4_pushed')",
            f"UPDATE vc_data SET payload = md5(payload || 'p') WHERE id <= {n}",
            VC_SQL.commit("v4 clone change"),
            "SELECT dolt_push('origin', 'v4_pushed')"], "v4-push", cluster=remote)
        return Outcome(metrics={"clone_ms": clone[0], "fetch_ms": fetch[0],
                                "push_ms": push[3]},
                       transactions=3, unit="operation")
