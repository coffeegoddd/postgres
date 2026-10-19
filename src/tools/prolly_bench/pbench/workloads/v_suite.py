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


@registry.workloads.register("V6")
class V6(VWorkload):
    """Reclamation: the space branches take and give back once deleted and
    the chunk store collected, and how long default and full collections
    take (dolt_gc)."""

    id, title = "V6", "Space and collection after deleting branches"

    # the chunk store's files, in bytes
    SPACE = ("SELECT sum((pg_stat_file(d || '/' || f)).size) FROM "
             "(SELECT 'base/' || oid AS d FROM pg_database "
             "WHERE datname = current_database()) db, pg_ls_dir(d) f "
             "WHERE f LIKE 'prolly\\_journal%' OR f LIKE 'prolly\\_table\\_%'")

    def own_variants(self, params):
        return [(f"b{b}-n{n}", {"branches": int(b), "changes": int(n)})
                for b, n in product(params.get("branches", [20]),
                                    params.get("changes", [10000]))]

    def _space(self, ctx) -> float:
        return int(ctx.cluster.scalar(self.SPACE, dbname=ctx.dbname) or 0) / 1048576

    @staticmethod
    def changed(b: int, n: int, rows: int) -> tuple[int, int]:
        """Branch b's n changed ids, from lo, wrapping past the last:
        (lo, past), ids in lo < id <= lo + n, or id <= past."""
        lo = b * n % rows
        return lo, lo + n - rows

    def operations(self, ctx):
        n, nb, rows = ctx.params["changes"], ctx.params["branches"], self.rows(ctx)
        if n > rows:
            raise ValueError("V6 changes exceed the dataset; refusing to clamp the workload")
        vc = VC_SQL
        stmts = ["SELECT dolt_gc()"]
        for b in range(nb):
            lo, past = self.changed(b, n, rows)
            stmts += [vc.branch(f"v6_b{b}"), vc.checkout(f"v6_b{b}"),
                      f"UPDATE vc_data SET payload = md5(payload) "
                      f"WHERE (id > {lo} AND id <= {lo + n}) OR id <= {past}",
                      vc.commit(f"v6 {b}"), vc.checkout("main")]
        timed_session(ctx, stmts, "v6-branches")
        with_branches = self._space(ctx)
        t = timed_session(ctx, ["SELECT dolt_gc()"], "v6-gc")
        collected = self._space(ctx)
        timed_session(ctx, [f"SELECT dolt_branch('-D', 'v6_b{b}')" for b in range(nb)],
                      "v6-delete")
        t += timed_session(ctx, ["SELECT dolt_gc()"], "v6-gc-deleted")
        after_default = self._space(ctx)
        t += timed_session(ctx, ["SELECT dolt_gc('--full')", "SELECT dolt_gc('--full')"],
                           "v6-full")
        return Outcome(metrics={"default_gc_ms": t[0],
                                "default_gc_after_delete_ms": t[1],
                                "full_gc_ms": t[2],
                                "full_gc_all_live_ms": t[3],
                                "space_with_branches_mb": with_branches,
                                "space_collected_mb": collected,
                                "space_after_delete_gc_mb": after_default,
                                "space_after_full_mb": self._space(ctx)},
                       transactions=4, unit="collection")


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

    def prepare(self, ctx):
        # Only the V data is committed: what a clone copies is the same with
        # or without a background workload (whose tables stay uncommitted).
        ctx.cluster.sql(f"""
            CREATE TABLE vc_data (id bigint PRIMARY KEY, payload text NOT NULL);
            INSERT INTO vc_data SELECT g, md5(g::text) FROM generate_series(1, {self.rows(ctx)}) g;
        """, dbname=ctx.dbname, options=ctx.session_options())
        timed_session(ctx, [VC_SQL.commit("initial data")], "prepare-commit")
        if ctx.params["background"] == "S1":
            pgbench_init(ctx, int(ctx.size_params["pgbench_scale"]))

    def measure(self, ctx, window):
        if ctx.params["changes"] > self.rows(ctx):
            raise ValueError("V4 changes exceed the dataset; refusing to clamp the workload")
        if ctx.make_cluster is None:
            raise NotImplementedByEngine("V4 needs a second instance, which this runner can't make")
        self._remote = ctx.make_cluster("v4-remote")
        # (the runner destroys it, after the collectors have read its writes)
        ctx.peers.append(self._remote)
        try:
            self._remote.sql(f"CREATE DATABASE {ctx.dbname}")
            # (the clone makes the versioned schema; its search path is the
            # origin's, as the target's setup sets it)
            schema = targets.for_am(ctx.table_am).schema
            if schema:
                self._remote.sql(f"ALTER DATABASE {ctx.dbname} SET search_path = {schema}, public")
            outcome = super().measure(ctx, window)
            outcome.notes.append("the window also holds the origin's and the clone's "
                                 "changes of n rows and their commits, made for the "
                                 "fetch and the push")
            self._check_moved(ctx)
            return outcome
        finally:
            self._save_log(ctx)
            self._remote = None

    def _save_log(self, ctx):
        log = getattr(self._remote, "log_file", None)
        if log is not None and log.exists():
            (ctx.raw_dir / "v4-remote-server.log").write_text(log.read_text())

    def _check_moved(self, ctx):
        """The fetch and the push moved what they were timed moving."""
        remote, db = self._remote, ctx.dbname
        fetched = remote.scalar("SELECT dolt_hashof('origin/v4_origin')", dbname=db)
        made = ctx.cluster.scalar("SELECT dolt_hashof('v4_origin')", dbname=db)
        if fetched != made:
            raise RuntimeError(f"V4's fetch didn't bring v4_origin ({fetched} != {made})")
        pushed = ctx.cluster.scalar("SELECT dolt_hashof('v4_pushed')", dbname=db)
        sent = remote.scalar("SELECT dolt_hashof('v4_pushed')", dbname=db)
        if pushed != sent:
            raise RuntimeError(f"V4's push didn't make v4_pushed ({pushed} != {sent})")

    def operations(self, ctx):
        remote = self._remote
        assert remote is not None, "V4's operations run inside its measure()"
        n = ctx.params["changes"]
        url = ctx.cluster.libpq_url(ctx.dbname)
        clone = timed_session(ctx, [f"SELECT dolt_clone('{url}')"], "v4-clone",
                              cluster=remote)
        wrong = targets.for_am(ctx.table_am).wrong_am(remote, ctx.dbname, ["vc_data"])
        if wrong:
            raise RuntimeError(f"V4's clone made vc_data on another access method: {wrong}")
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
