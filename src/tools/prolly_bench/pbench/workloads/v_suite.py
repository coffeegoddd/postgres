"""The V suite: version-control workloads, for prolly engines only.  They
call the prolly_* SQL functions.

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

from pbench.core import registry
from pbench.core.interfaces import Context, Outcome, Window, Workload
from pbench.workloads._common import BackgroundPgbench, pgbench_init, timed_session


def _median(xs: list[float]) -> float | None:
    return statistics.median(xs) if xs else None


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
        timed_session(ctx, ["SELECT prolly_commit('initial data')"], "prepare-commit")

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
        stmts = []
        for i in range(self.iterations(ctx)):
            stmts += [f"SELECT prolly_branch_create('v1_b{i}', NULL)",
                      f"SELECT prolly_checkout('v1_b{i}')",
                      "SELECT prolly_checkout('main')"]
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
        stmts = []
        for i in range(self.iterations(ctx)):
            stmts += [f"UPDATE vc_data SET payload = md5(payload || '{i}') "
                      f"WHERE id <= {n}",
                      f"SELECT prolly_commit('v2 {i}')"]
        t = timed_session(ctx, stmts, "v2")
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
        stmts = [
            "SELECT prolly_branch_create('v3_other', NULL)",
            "SELECT prolly_checkout('v3_other')",
            f"UPDATE vc_data SET payload = 'other' WHERE {self._ids(ctx, 1)}",
            "SELECT prolly_commit('v3 other side')",
            "SELECT prolly_checkout('main')",
            f"UPDATE vc_data SET payload = 'main' WHERE {self._ids(ctx, 0)}",
            "SELECT prolly_commit('v3 main side')",
            "SELECT count(*) FROM prolly_diff('main', 'v3_other')",
            "SELECT prolly_merge('v3_other')",
        ]
        t = timed_session(ctx, stmts, "v3")
        return Outcome(metrics={"diff_ms": t[7], "merge_ms": t[8]},
                       transactions=2, unit="operation")


@registry.workloads.register("V4")
class V4(VWorkload):
    """Clone, then fetch and push, between this instance and a second local
    instance."""

    id, title = "V4", "Clone, fetch and push between two instances"

    def operations(self, ctx):
        assert ctx.make_cluster is not None
        remote = ctx.make_cluster("remote")
        try:
            remote.sql(f"CREATE DATABASE {ctx.dbname}")
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
