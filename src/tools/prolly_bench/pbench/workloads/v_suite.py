"""The V suite: version-control workloads, for prolly engines only.  The
tree engines (prolly_d1) are driven through the prolly_* SQL functions,
page-based prolly through the dolt_* ones (its tables are in a versioned
schema, committed with dolt_commit).

Every V workload has a "background" variant dimension: "none", or "S1" to
run pgbench TPC-B alongside the version-control operations, measuring how
each affects the other.  With background S1 the client count is the number
of background pgbench clients; without it, only one client count is run.
"""

from __future__ import annotations

import statistics
from contextlib import nullcontext
from itertools import product
from typing import Any

from pbench.core import registry, targets
from pbench.core.interfaces import Context, NotImplementedByEngine, Outcome, Window, Workload
from pbench.workloads._common import BackgroundPgbench, pgbench_init, timed_session


def _median(xs: list[float]) -> float | None:
    return statistics.median(xs) if xs else None


class _TreeSQL:
    """Version-control statements of the tree engines."""

    def branch(self, name):
        return f"SELECT prolly_branch_create('{name}', NULL)"

    def checkout(self, name):
        return f"SELECT prolly_checkout('{name}')"

    def commit(self, message):
        return f"SELECT prolly_commit('{message}')"

    def diff_count(self, a, b, table):
        return f"SELECT count(*) FROM prolly_diff('{a}', '{b}')"

    def merge(self, name):
        return f"SELECT prolly_merge('{name}')"


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


def vc_sql(ctx: Context):
    return _DoltSQL() if ctx.table_am in targets.VERSIONED_ONLY else _TreeSQL()


class VWorkload(Workload):
    suite = "V"

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
        timed_session(ctx, [vc_sql(ctx).commit("initial data")], "prepare-commit")

    def background(self, ctx: Context):
        if ctx.params["background"] == "S1":
            return BackgroundPgbench(ctx, "tpcb-like", ctx.key.clients, "background")
        if ctx.params["background"] != "none":
            raise ValueError(f"unknown background workload {ctx.params['background']}")
        return nullcontext()

    def measure(self, ctx: Context, window: Window) -> Outcome:
        with window, self.background(ctx) as bg:
            outcome = self.operations(ctx)
        if bg is not None:
            outcome.metrics["background_tps"] = bg.tps()
        return outcome

    def operations(self, ctx: Context) -> Outcome:
        raise NotImplementedError

    def tables(self, ctx):
        return ["vc_data"]

    def iterations(self, ctx: Context) -> int:
        return int(ctx.params.get("iterations", 10))


@registry.workloads.register("V1")
class V1(VWorkload):
    id, title = "V1", "Branch create and checkout"

    def operations(self, ctx):
        vc = vc_sql(ctx)
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

    def operations(self, ctx):
        n = min(ctx.params["changes"], self.rows(ctx))
        vc = vc_sql(ctx)
        # on a branch of its own, so a background workload's writes (on
        # main) aren't in its commits
        stmts = [vc.branch("v2_work"), vc.checkout("v2_work")]
        for i in range(self.iterations(ctx)):
            stmts += [f"UPDATE vc_data SET payload = md5(payload || '{i}') "
                      f"WHERE id <= {n}",
                      vc.commit(f"v2 {i}")]
        t = timed_session(ctx, stmts, "v2")[2:]
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
        if ctx.params["locality"] == "clustered":
            lo = 1 + side * (n - overlap)
            return f"id BETWEEN {lo} AND {lo + n - 1}"
        stride = max(1, rows // (2 * n))
        offset = 0 if side == 0 else (stride // 2 or 1)
        pred = f"id % {stride} = {offset} AND id <= {stride * n}"
        if overlap and side == 1:
            pred = f"({pred}) OR id % {stride} = 0 AND id <= {stride * overlap}"
        return pred

    def operations(self, ctx):
        # Both sides are branches of their own, so a background workload's
        # writes (on main) are on neither and the merge doesn't touch them.
        vc = vc_sql(ctx)
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
    instance."""

    id, title = "V4", "Clone, fetch and push between two instances"

    def operations(self, ctx):
        if ctx.table_am in targets.VERSIONED_ONLY:
            raise NotImplementedByEngine("prolly has no clone, fetch or push yet")
        assert ctx.make_cluster is not None
        remote = ctx.make_cluster("remote")
        try:
            remote.sql(f"CREATE DATABASE {ctx.dbname}")
            setup = targets.for_am(ctx.table_am).setup_sql(ctx.dbname)
            if setup:
                remote.sql(setup, dbname=ctx.dbname)
            local_url = ctx.cluster.libpq_url(ctx.dbname)
            clone = timed_session(ctx, [f"SELECT prolly_clone('{local_url}')"],
                                  "v4-clone", cluster=remote)
            timed_session(ctx, [
                "UPDATE vc_data SET payload = 'local' WHERE id <= 1000",
                "SELECT prolly_commit('v4 local change')"], "v4-local-change")
            fetch = timed_session(ctx, [f"SELECT prolly_fetch('{local_url}', 'main')"],
                                  "v4-fetch", cluster=remote)
            remote_url = remote.libpq_url(ctx.dbname)
            push = timed_session(ctx, [f"SELECT prolly_push('{remote_url}', 'main')"],
                                 "v4-push")
        finally:
            remote.destroy()
        return Outcome(metrics={"clone_ms": clone[0], "fetch_ms": fetch[0],
                                "push_ms": push[0]},
                       transactions=3, unit="operation")
