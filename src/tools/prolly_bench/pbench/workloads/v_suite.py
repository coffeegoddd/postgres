"""The V suite: version-control workloads, for prolly, driven through the
dolt_* SQL functions (its tables are in a versioned schema, committed with
dolt_commit).

Every V workload has a "background" variant dimension: "none", or "S1" to
run pgbench TPC-B alongside the version-control operations, measuring how
each affects the other.  With background S1 the client count is the number
of background pgbench clients; without it, only one client count is run.
"""

from __future__ import annotations

import json
import math
import statistics
import subprocess
import time
from contextlib import nullcontext
from itertools import product
from typing import Any

from pbench import vgrowth, vshape
from pbench.core import registry, targets
from pbench.core.sqlerr import raise_for_sql_error
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


@registry.workloads.register("V7")
class V7(VWorkload):
    """Commit per transaction: K one-row updates, each committed as it is
    made, and the counters read after each.  The work each commit does
    should stay flat however many came before it; vgrowth gates the last
    tenth's mean per transaction against the first's.

    Variants: "shared" and "owned" commit with dolt.transaction_commit in
    that writer mode; "manual" with a dolt_commit after each transaction,
    autovacuum on; "pinned" as manual with another session holding a
    snapshot (the vacuum horizon) for the whole run; "status" as owned
    with a dolt_status every 100 transactions."""

    id, title = "V7", "Commit per transaction"

    VARIANTS = {
        "shared": {"mode": "shared", "transaction_commit": True},
        "owned": {"mode": "owned", "transaction_commit": True},
        "manual": {"mode": "shared", "transaction_commit": False, "autovacuum": True},
        "pinned": {"mode": "shared", "transaction_commit": False, "autovacuum": True,
                   "pin": True},
        "status": {"mode": "owned", "transaction_commit": True, "status_every": 100},
    }

    COUNTERS = ("SELECT 'v7|' || json_build_object("
                "'wal_bytes', (SELECT wal_bytes FROM pg_stat_wal), "
                "'buffer_accesses', (SELECT sum(hits + reads) FROM pg_stat_io "
                "WHERE backend_type = 'client backend' AND object = 'relation'), "
                "'p', (SELECT json_object_agg(name, value) FROM pg_stat_prolly))")

    def own_variants(self, params):
        return [(v, {"v7": v}) for v in params.get("commit_modes", list(self.VARIANTS))]

    def commits(self, ctx) -> int:
        return int(ctx.params.get("commits", 2000))

    def prepare(self, ctx):
        spec = self.VARIANTS[ctx.params["v7"]]
        super().prepare(ctx)
        ctx.cluster.sql(f"SELECT dolt_writer_mode('{spec['mode']}')", dbname=ctx.dbname)
        # (the load's change-log entries, consumed by its commit, vacuumed
        # away: else every commit walks them, and what the run adds is lost
        # beside them)
        ctx.cluster.sql("VACUUM vc_data", dbname=ctx.dbname)
        if spec.get("autovacuum"):
            # (a worker every second, so a log is vacuumed within the run,
            # as it is soon after a fold consumes its entries: a held
            # horizon, which keeps them, then shows)
            ctx.cluster.sql("ALTER SYSTEM SET autovacuum = on; "
                            "ALTER SYSTEM SET autovacuum_naptime = '1s'; "
                            "SELECT pg_reload_conf()")

    def _pin(self, ctx):
        """A session idle in a transaction with a snapshot: the horizon held."""
        proc = subprocess.Popen(
            [ctx.cluster.bin("psql"), "-X", "-q", "-d", ctx.dbname],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            text=True, env=ctx.cluster.client_env(ctx.session_options()))
        assert proc.stdin is not None
        proc.stdin.write("BEGIN ISOLATION LEVEL REPEATABLE READ;\nSELECT count(*) FROM vc_data;\n")
        proc.stdin.flush()
        for _ in range(100):
            if ctx.cluster.scalar("SELECT count(*) FROM pg_stat_activity WHERE "
                                  "state = 'idle in transaction' AND backend_xmin IS NOT NULL",
                                  dbname=ctx.dbname) != "0":
                return proc
            time.sleep(0.1)
        proc.kill()
        raise RuntimeError("V7's pinning session took no snapshot")

    def operations(self, ctx):
        spec = self.VARIANTS[ctx.params["v7"]]
        k, rows = self.commits(ctx), self.rows(ctx)
        if k < 20:
            raise ValueError("V7 needs at least 20 transactions to compare tenths")
        stmts = []
        if spec["transaction_commit"]:
            stmts.append("SET dolt.transaction_commit = on")
        stmts += ["SELECT pg_stat_force_next_flush()", self.COUNTERS]
        for i in range(k):
            stmts.append(f"UPDATE vc_data SET payload = md5(payload) "
                         f"WHERE id = {i * 7919 % rows + 1}")
            if not spec["transaction_commit"]:
                stmts.append(VC_SQL.commit(f"v7 {i}"))
            every = spec.get("status_every")
            if every and (i + 1) % every == 0:
                stmts.append("SELECT count(*) FROM dolt_status")
            stmts += ["SELECT pg_stat_force_next_flush()", self.COUNTERS]
        script = "\n".join(s + ";" for s in stmts) + "\n"
        (ctx.raw_dir / "v7.sql").write_text(script)
        pin = self._pin(ctx) if spec.get("pin") else None
        try:
            res = ctx.cluster.sql(script, dbname=ctx.dbname, options=ctx.session_options(),
                                  check=False)
        finally:
            if pin is not None:
                pin.kill()
                pin.wait()
        (ctx.raw_dir / "v7.out").write_text(res.stdout + res.stderr)
        raise_for_sql_error(res, script)
        readings = [json.loads(line[3:]) for line in res.stdout.splitlines()
                    if line.startswith("v7|")]
        if len(readings) != k + 1:
            raise RuntimeError(f"V7 expected {k + 1} counter readings, got {len(readings)}")
        series: dict = {}
        for c in vgrowth.FLOORS:
            # (a counter the server doesn't report is missing, not zero: a
            # renamed one would otherwise stop being gated unseen)
            values = [r[c] if c in vgrowth.SERVER_COUNTERS else (r["p"] or {}).get(c)
                      for r in readings]
            series[c] = None if any(v is None for v in values) else \
                [float(b) - float(a) for a, b in zip(values, values[1:])]
        (ctx.raw_dir / "v7-series.json").write_text(json.dumps(series) + "\n")
        ungated = vgrowth.AUTOVACUUM_COUNTED if spec.get("autovacuum") else set()
        return Outcome(metrics=vgrowth.growth_metrics(series, ungated),
                       transactions=k, unit="transaction")


@registry.workloads.register("V9")
class V9(VWorkload):
    """Growth shapes: each version-control operation's work, counted in
    pg_stat_prolly, at four or more table sizes N (the "rows" sweep, Δ
    fixed), four or more change sizes Δ (the "delta" sweep, N fixed) and
    four or more counts K of unrelated objects (the "catalog" sweep, N and
    Δ fixed), each point on a fresh database.  vshape fits each counter's
    growth and gates it against the operation's target (vshape.TARGETS).

    The catalog sweep's unrelated objects are K shared tables in a plain
    schema and K tables on another branch, made from a commit before the
    table, so that it holds no copies of it: what an operation reads of
    the catalogs (catalog_blocks_read, the block fetches of every system
    catalog and index) is to be the table's, whatever else they hold.

    The table is narrow and filled to a tenth of each page (fillfactor
    10), so N rows are about N / 10 blocks: block-map work shows its shape
    at sizes CI can afford.  An index on the column the changes update
    keeps them from being heap-only, so rows written back make index
    entries.  The Δ changed rows are spread evenly over the keys.

    Each point runs, counting each operation alone (the counters read
    after the statement before it and after it): a branch made from
    the load commit and first used (first_use); Δ rows changed on it;
    another branch made from it, dirty (create_branch); a checkout of
    main, used and clean, from the dirty branch (checkout_used); back;
    the Δ rows committed (commit); the commit's diff (diff); the load
    commit mounted (mount) and a point read of it by its key (point_read);
    Δ rows changed again, the head reset
    softly to the load commit (soft_reset) and hard, discarding the
    working set (hard_reset); Δ rows committed again, main changed in
    another row, and the branch merged into main (merge); the branch
    deleted (branch_delete)."""

    id, title = "V9", "Growth shapes"

    def variants(self, params):
        # (counters alone: never with a background workload)
        return [("bgnone", {"background": "none"})]

    def prepare(self, ctx):
        """Nothing: each point makes its own database."""

    def tables(self, ctx):
        return []

    def sweeps(self, ctx) -> dict[str, list[tuple[int, int, int]]]:
        """Each sweep's points, (rows, delta, unrelated objects)."""
        p = ctx.params
        rows = [int(n) for n in p.get("rows", [4000, 16000, 64000, 256000])]
        deltas = [int(d) for d in p.get("deltas", [700, 2800, 11200, 44800])]
        others = [int(k) for k in p.get("others", [32, 128, 512, 2048])]
        delta, fixed = int(p.get("delta", 10)), int(p.get("delta_rows", 64000))
        others_rows = int(p.get("others_rows", 4000))
        for n in rows + [others_rows]:
            if delta >= n:
                raise ValueError(f"V9's delta {delta} must be fewer than its rows {n}")
        for d in deltas:
            if d >= fixed:
                raise ValueError(f"V9's delta {d} must be fewer than delta_rows {fixed}")
        for levels in (rows, deltas, others):
            if len(levels) < 4 or min(levels) <= 0 or max(levels) / min(levels) < 8:
                raise ValueError("each V9 sweep needs four levels or more spanning 8x")
        return {"rows": [(n, delta, 0) for n in rows],
                "delta": [(fixed, d, 0) for d in deltas],
                "catalog": [(others_rows, delta, k) for k in others]}

    # (the catalogs' block fetches summed over every OID they can have, so
    # that the reading reads no catalog itself)
    READ = ("SELECT pg_stat_force_next_flush();\n"
            "SELECT 'v9|{label}|' || json_object_agg(name, value) FROM ("
            "SELECT name, value FROM pg_stat_prolly UNION ALL "
            "SELECT 'catalog_blocks_read', sum(pg_stat_get_blocks_fetched(o::oid))::bigint "
            "FROM generate_series(1, 16383) o) r;\n")

    @staticmethod
    def unrelated(k: int) -> str:
        """K shared tables in a plain schema and K tables on another branch,
        made from a commit before the table's (committed a hundred at a
        time, so that no transaction holds the locks of them all)."""
        table = "(id int PRIMARY KEY, a text, b int, c date)"
        return f"""
            CREATE TABLE v9_first (id int PRIMARY KEY);
            {VC_SQL.commit('v9 start')};
            CREATE SCHEMA v9_plain;
            DO $$ BEGIN FOR g IN 1..{k} LOOP
                EXECUTE format('CREATE TABLE v9_plain.p%s {table}', g);
                IF g % 100 = 0 THEN COMMIT; END IF;
            END LOOP; END $$;
            SELECT dolt_branch('v9_other');
            SELECT dolt_checkout('v9_other');
            DO $$ BEGIN FOR g IN 1..{k} LOOP
                EXECUTE format('CREATE TABLE v9_o%s {table}', g);
                IF g % 100 = 0 THEN COMMIT; END IF;
            END LOOP; END $$;
            SELECT dolt_checkout('main');
        """

    @staticmethod
    def change(n: int, d: int, value: int) -> str:
        step = n // d
        return (f"UPDATE v9 SET v = {value} WHERE id IN "
                f"(SELECT 1 + i * {step} FROM generate_series(0, {d - 1}) i)")

    def steps(self, n: int, d: int) -> list[tuple[str, str]]:
        """The point's statements, each (label, sql): those labelled are
        counted."""
        return [
            ("", "SELECT dolt_branch('v9_b')"),
            ("first_use", "SELECT dolt_checkout('v9_b')"),
            ("", self.change(n, d, 1)),
            ("create_branch", "SELECT dolt_branch('v9_c')"),
            ("checkout_used", "SELECT dolt_checkout('main')"),
            ("", "SELECT dolt_checkout('v9_b')"),
            ("commit", "SELECT dolt_commit('-am', 'v9 changes')"),
            ("diff", "SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::v9)"),
            ("mount", "SELECT prolly_mount('HEAD~1', current_schema(), 'v9m', ARRAY['v9'])"),
            ("point_read", "SELECT v FROM v9m.v9 WHERE id = 1"),
            ("", "SELECT prolly_unmount('v9m')"),
            ("", self.change(n, d, 2)),
            ("soft_reset", "SELECT dolt_reset('--soft', 'HEAD~1')"),
            ("hard_reset", "SELECT dolt_reset('--hard')"),
            ("", self.change(n, d, 3)),
            ("", "SELECT dolt_commit('-am', 'v9 changes again')"),
            ("", "SELECT dolt_checkout('main')"),
            # (a row no change touches: the last, past every changed one)
            ("", f"UPDATE v9 SET v = 9 WHERE id = {n}"),
            ("", "SELECT dolt_commit('-am', 'v9 on main')"),
            ("merge", "SELECT dolt_merge('v9_b')"),
            ("branch_delete", "SELECT dolt_branch('-D', 'v9_b')"),
        ]

    def point(self, ctx, sweep: str, n: int, d: int, k: int = 0) -> dict[str, dict[str, int]]:
        """One point's counted operations: {label: {counter: growth}}."""
        db = f"v9_{sweep}_{n}_{d}_{k}"
        target = targets.for_am(ctx.table_am)
        ctx.cluster.sql(f"CREATE DATABASE {db}")
        try:
            setup = target.setup_sql(db)
            if setup:
                ctx.cluster.sql(setup, dbname=db)
            if k:
                ctx.cluster.sql(self.unrelated(k), dbname=db, options=ctx.session_options())
            ctx.cluster.sql(f"""
                CREATE TABLE v9 (id bigint PRIMARY KEY, v int NOT NULL, payload text NOT NULL)
                    WITH (fillfactor = 10, autovacuum_enabled = off);
                INSERT INTO v9 SELECT g, 0, md5(g::text) FROM generate_series(1, {n}) g;
                CREATE INDEX ON v9 (v);
                {VC_SQL.commit('v9 load')};
            """, dbname=db, options=ctx.session_options())
            script = ""
            for label, sql in self.steps(n, d):
                if label:
                    script += self.READ.format(label="before")
                script += sql + ";\n"
                if label:
                    script += self.READ.format(label=label)
            (ctx.raw_dir / f"v9-{sweep}-{n}-{d}-{k}.sql").write_text(script)
            res = ctx.cluster.sql(script, dbname=db, options=ctx.session_options(), check=False)
            (ctx.raw_dir / f"v9-{sweep}-{n}-{d}-{k}.out").write_text(res.stdout + res.stderr)
            raise_for_sql_error(res, script)
        finally:
            ctx.cluster.sql(f"DROP DATABASE {db} WITH (FORCE)")
        readings = [line.split("|", 2)[1:] for line in res.stdout.splitlines()
                    if line.startswith("v9|")]
        out: dict[str, dict[str, int]] = {}
        for (before, a), (label, b) in zip(readings[::2], readings[1::2]):
            if before != "before":
                raise RuntimeError(f"V9's readings are out of order at {label}")
            a, b = json.loads(a), json.loads(b)
            out[label] = {name: int(b[name]) - int(a[name]) for name in b}
        expected = [label for label, _ in self.steps(n, d) if label]
        if list(out) != expected:
            raise RuntimeError(f"V9 expected readings of {expected}, got {list(out)}")
        return out

    def operations(self, ctx):
        series: dict = {}
        metrics: dict = {}
        points = 0
        for sweep, pts in self.sweeps(ctx).items():
            levels = [{"rows": n, "delta": d, "catalog": k}[sweep] for n, d, k in pts]
            counted = [self.point(ctx, sweep, n, d, k) for n, d, k in pts]
            points += len(pts)
            checks: dict = {}
            for label in counted[0]:
                # (a counter the server doesn't report is missing, not 0)
                names = set.intersection(*(set(c[label]) for c in counted))
                checks[label] = {name: [c[label][name] for c in counted] for name in sorted(names)}
                for name in vshape.WORK + vshape.CATALOG:
                    if name in checks[label]:
                        b = vshape.growth(levels, checks[label][name], vshape.FLOORS[name])
                        metrics[f"{label}_{sweep}_{name}_growth"] = b
            series[sweep] = {"levels": levels, "points": [list(p) for p in pts], "checks": checks}
        (ctx.raw_dir / "v9-series.json").write_text(json.dumps(series, indent=1) + "\n")
        return Outcome(metrics=metrics, transactions=points, unit="point")


@registry.workloads.register("V8")
class V8(VWorkload):
    """Storage by component (#500): prolly_storage_breakdown(), the
    database's size, and a heap table's given the same rows and updates,
    read after each stage: the load, its first commit, K commits each
    updating P% of the rows, dolt_gc with prolly_page_sweep(), and a VACUUM
    with another sweep (what vacuuming the change log and the pages gives
    back).  Sizes only: no timings."""

    id, title = "V8", "Storage by component"

    STAGES = ("load", "commit", "updates", "gc", "vacuum")

    READ = ("CHECKPOINT;\n"
            "SELECT 'v8|' || json_build_object("
            "'breakdown', (SELECT row_to_json(b) FROM prolly_storage_breakdown() b), "
            "'database_bytes', pg_database_size(current_database()), "
            "'table_bytes', pg_total_relation_size('vc_data'), "
            "'heap_bytes', pg_total_relation_size('public.v8_heap'));\n")

    def commits(self, ctx) -> int:
        return int(ctx.params.get("commits", 10))

    def update_percent(self, ctx) -> float:
        return float(ctx.params.get("update_percent", 1))

    def variants(self, params):
        # (storage read alone: never with a background workload, whatever
        # the suite's backgrounds, so a plain `run V` plans no cell of it
        # that can't run)
        return [(f"{label}-bgnone" if label else "bgnone", {**extra, "background": "none"})
                for label, extra in self.own_variants(params)]

    @staticmethod
    def update_where(i: int, changed: int, n: int) -> str:
        """Commit i's rows: the changed ids after (i * changed) mod n,
        wrapping past the table's end to its start, so each commit updates
        as many."""
        lo = i * changed % n
        hi = lo + changed
        return f"WHERE (id > {lo} AND id <= {hi}) OR id <= {hi - n}"

    def prepare(self, ctx):
        # (the tables, empty: the load is the first stage read)
        self._sql(ctx, """
            CREATE TABLE vc_data (id bigint PRIMARY KEY, payload text NOT NULL);
            CREATE TABLE public.v8_heap (id bigint PRIMARY KEY, payload text NOT NULL)
                USING heap;
        """)

    def _sql(self, ctx, sql: str) -> str:
        return ctx.cluster.sql(sql, dbname=ctx.dbname, options=ctx.session_options()).stdout

    def _read(self, ctx) -> dict:
        lines = [x for x in self._sql(ctx, self.READ).splitlines() if x.startswith("v8|")]
        if len(lines) != 1:
            raise RuntimeError(f"V8 expected one reading, got {len(lines)}")
        return json.loads(lines[0][3:])

    def operations(self, ctx):
        n, k = self.rows(ctx), self.commits(ctx)
        changed = max(1, int(n * self.update_percent(ctx) / 100))
        readings: dict = {}
        self._sql(ctx, f"""
            INSERT INTO vc_data SELECT g, md5(g::text) FROM generate_series(1, {n}) g;
            INSERT INTO public.v8_heap SELECT g, md5(g::text) FROM generate_series(1, {n}) g;
        """)
        readings["load"] = self._read(ctx)
        self._sql(ctx, VC_SQL.commit("v8 load"))
        readings["commit"] = self._read(ctx)
        for i in range(k):
            where = self.update_where(i, changed, n)
            self._sql(ctx, f"UPDATE vc_data SET payload = md5(payload) {where};\n"
                           f"UPDATE public.v8_heap SET payload = md5(payload) {where};\n"
                           f"{VC_SQL.commit(f'v8 {i}')};")
        readings["updates"] = self._read(ctx)
        self._sql(ctx, "SELECT dolt_gc(); SELECT prolly_page_sweep();")
        readings["gc"] = self._read(ctx)
        self._sql(ctx, "VACUUM vc_data; VACUUM public.v8_heap; CHECKPOINT;\n"
                       "SELECT prolly_page_sweep();")
        readings["vacuum"] = self._read(ctx)
        (ctx.raw_dir / "v8-breakdown.json").write_text(json.dumps(readings, indent=1) + "\n")
        return Outcome(metrics=self.metrics(readings), transactions=len(self.STAGES),
                       unit="stage")

    # the breakdown's columns that are files, apart: prolly's storage
    FILES = ("vm_bytes", "fsm_bytes", "page_store_bytes", "block_map_bytes",
             "journal_bytes", "settled_bytes", "unsettled_bytes", "chunk_store_other_bytes")

    @classmethod
    def metrics(cls, readings: dict) -> dict:
        """Each stage's reading, flat: <stage>_<column> in bytes; prolly's
        files summed (<stage>_prolly_bytes), and over the heap table's
        (<stage>_prolly_over_heap).  The database's size holds the heap
        table too, and the catalogs."""
        out: dict = {}
        for stage in cls.STAGES:
            r = readings[stage]
            b = r["breakdown"] or {}
            for col, v in b.items():
                out[f"{stage}_{col}"] = None if v is None else float(v)
            for col in ("database_bytes", "table_bytes", "heap_bytes"):
                out[f"{stage}_{col}"] = float(r[col])
            files = [b.get(c) for c in cls.FILES]
            total = None if any(v is None for v in files) else float(sum(files))
            out[f"{stage}_prolly_bytes"] = total
            out[f"{stage}_prolly_over_heap"] = (
                total / float(r["heap_bytes"]) if total is not None and r["heap_bytes"] else None)
        return out
