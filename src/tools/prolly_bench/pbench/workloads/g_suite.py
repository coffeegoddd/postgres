"""The G and GV suites: PostGIS.

G (heap against prolly, as S) measures spatial storage: bulk loads and index
builds, window queries, KNN, spatial joins, moving objects, large
geometries, vector tiles, geography, and ANALYZE.  GV (prolly only, as V)
measures spatial version control: commits, diffs and merges, pushes,
clones and fetches, growth over many commits, and checkouts.

Every table is made by a seeded synthetic generator in SQL (no data is
downloaded): clustered points, small rotated buildings, zones with many
vertices (stored out of line), large polygons with tens of thousands of
vertices, a fleet of points, and geography points.  The [sizes] keys in
SIZE_DEFAULTS scale it, so both size classes come from one generator.

Every G workload records its answers in raw/answers.json: an md5 of each
of a fixed set of queries' results, over EWKB for geometries.  A prolly
run whose answers differ from the heap run's isn't comparable with it
(validation.compatible), whatever its timing.
"""

from __future__ import annotations

import json
import statistics
import time
from contextlib import nullcontext
from itertools import product
from typing import Any

from pbench.core import registry, targets
from pbench.core.interfaces import Context, NotImplementedByEngine, Outcome, Workload
from pbench.workloads._common import BackgroundPgbench, run_pgbench, timed_session

# The generator's extent: EPSG:3857 metres, [0, EXTENT)^2.
EXTENT = 1_000_000
SRID = 3857
CENTERS = 32
ZONES_PER_SIDE = 8

# [sizes] keys and their defaults (a tiny run's).
SIZE_DEFAULTS = {
    "g_points": 20000,
    "g_buildings": 5000,
    "g_large": 4,
    "g_large_vertices": 20000,
    "g_fleet": 2000,
    "g_geog": 10000,
}
SIZE_KEYS = frozenset(SIZE_DEFAULTS)

# Per-workload parameter keys (suites/g.py registers them).
PARAMS = {
    "G1": set(), "G2": {"predicates", "window"}, "G3": {"k"}, "G4": {"parallel", "iterations"},
    "G5": set(), "G6": {"iterations"}, "G7": set(), "G8": {"radius"}, "G9": set(),
    "GV1": {"background", "changes", "iterations"}, "GV2": {"background", "changes", "overlap"},
    "GV3": {"background", "changes"}, "GV4": {"background", "commits"},
    "GV5": {"background", "iterations"},
}
# pgbench workloads: a fixed transaction count and an offered rate apply.
PGBENCH = frozenset({"G2", "G3", "G5", "G8"})


def size(ctx: Context, key: str) -> int:
    return int(ctx.size_params.get(key, SIZE_DEFAULTS[key]))


def u(expr: str, salt: int) -> str:
    """SQL for a uniform [0, 1) double from an integer expression: a
    multiplicative hash, the same on every platform."""
    return (f"((({expr})::bigint * 2654435761 + {salt} * 40503 + 12345) "
            f"% 4294967296)::float8 / 4294967296")


def _sql(ctx: Context, sql: str):
    return ctx.cluster.sql(sql, dbname=ctx.dbname, options=ctx.session_options())


def _scalar(ctx: Context, sql: str) -> str:
    return ctx.cluster.scalar(sql, dbname=ctx.dbname)


def _median(xs: list[float]) -> float | None:
    return statistics.median(xs) if xs else None


def _p95(xs: list[float]) -> float | None:
    if not xs:
        return None
    s = sorted(xs)
    return s[min(len(s) - 1, int(0.95 * len(s)))]


# ---------------------------------------------------------------------------
# The generator
# ---------------------------------------------------------------------------

def point_sql(id_expr: str) -> str:
    """A point clustered around one of CENTERS centres."""
    c = f"({id_expr}) % {CENTERS}"
    spread = EXTENT * 0.04
    return (f"ST_SetSRID(ST_MakePoint("
            f"{EXTENT * 0.05} + {EXTENT * 0.9} * {u(c, 1)} + {spread} * ({u(id_expr, 3)} - 0.5) * {u(id_expr, 5)} * 2, "
            f"{EXTENT * 0.05} + {EXTENT * 0.9} * {u(c, 2)} + {spread} * ({u(id_expr, 4)} - 0.5) * {u(id_expr, 6)} * 2"
            f"), {SRID})")


def _large_select(ctx: Context) -> str:
    v = size(ctx, "g_large_vertices")
    radius = (f"r * (1 + 0.1 * sin(7 * t + n) + 0.05 * sin(53 * t) + 0.02 * sin(311 * t))")
    return f"""
        SELECT n, ST_MakePolygon(ST_MakeLine(array_agg(ST_SetSRID(ST_MakePoint(
                 cx + {radius} * cos(t), cy + {radius} * sin(t)), {SRID}) ORDER BY k)))
        FROM generate_series(1, {size(ctx, 'g_large')}) n,
             LATERAL (SELECT {EXTENT} * (0.2 + 0.6 * {u('n', 9)}) AS cx,
                             {EXTENT} * (0.2 + 0.6 * {u('n', 10)}) AS cy,
                             {EXTENT * 0.08} AS r) c,
             LATERAL (SELECT k, 2 * pi() * (k % {v}) / {v} AS t FROM generate_series(0, {v}) k) s
        GROUP BY n, cx, cy, r"""


# Each table: its columns, and the query that makes its rows.
TABLES = {
    "gpts": (f"id bigint PRIMARY KEY, g geometry(Point, {SRID}) NOT NULL",
             lambda ctx: f"SELECT i, {point_sql('i')} FROM generate_series(1, {size(ctx, 'g_points')}) i"),
    # small rectangles, 5 to 30 m a side, rotated
    "gbld": (f"id bigint PRIMARY KEY, g geometry(Polygon, {SRID}) NOT NULL",
             lambda ctx: f"""SELECT i, ST_Rotate(ST_Expand(p, 5 + 25 * {u('i', 7)}), pi() * {u('i', 8)}, p)
                 FROM generate_series(1, {size(ctx, 'g_buildings')}) i,
                      LATERAL (SELECT {point_sql('i + 7777777')} AS p) s"""),
    # a grid of zones, each edge segmented every 250 m (about 2000
    # vertices, stored out of line)
    "gzones": (f"id int PRIMARY KEY, g geometry(Polygon, {SRID}) NOT NULL",
               lambda ctx: f"""SELECT x * {ZONES_PER_SIDE} + y + 1,
                   ST_Segmentize(ST_MakeEnvelope(x * {EXTENT // ZONES_PER_SIDE}, y * {EXTENT // ZONES_PER_SIDE},
                                                 (x + 1) * {EXTENT // ZONES_PER_SIDE},
                                                 (y + 1) * {EXTENT // ZONES_PER_SIDE}, {SRID}), 250)
                   FROM generate_series(0, {ZONES_PER_SIDE - 1}) x, generate_series(0, {ZONES_PER_SIDE - 1}) y"""),
    # "coastlines": closed rings whose radius wanders by three sine terms
    "glarge": (f"id int PRIMARY KEY, g geometry(Polygon, {SRID}) NOT NULL", _large_select),
    "gfleet": (f"id bigint PRIMARY KEY, g geometry(Point, {SRID}) NOT NULL",
               lambda ctx: f"""SELECT i, ST_SetSRID(ST_MakePoint({EXTENT} * {u('i', 11)}, {EXTENT} * {u('i', 12)}), {SRID})
                   FROM generate_series(1, {size(ctx, 'g_fleet')}) i"""),
    "ggeog": ("id bigint PRIMARY KEY, g geography(Point, 4326) NOT NULL",
              lambda ctx: f"""SELECT i, ST_MakePoint(-180 + 360 * {u('i', 13)}, -85 + 170 * {u('i', 14)})::geography
                  FROM generate_series(1, {size(ctx, 'g_geog')}) i"""),
}


def load(ctx: Context, tables: list[str], indexes: bool = True) -> None:
    for t in tables:
        columns, rows = TABLES[t]
        _sql(ctx, f"CREATE TABLE {t} ({columns});\nINSERT INTO {t} {rows(ctx)};")
        if indexes:
            _sql(ctx, f"CREATE INDEX {t}_g ON {t} USING gist (g)")
    _sql(ctx, "ANALYZE " + ", ".join(tables))


def digest_sql(table: str, where: str = "") -> str:
    """Rows and an md5 over id:EWKB in id order."""
    return (f"SELECT count(*) || ':' || coalesce(md5(string_agg(id || ':' || "
            f"encode(ST_AsEWKB(g::geometry), 'hex'), ',' ORDER BY id)), '') FROM {table} {where}")


def answers(ctx: Context, queries: dict[str, str], values: dict[str, str] | None = None,
            cluster=None, dbname: str | None = None) -> dict[str, str]:
    """Each query's result as an md5 of its rows (as text, in C order), and
    values as they are, merged into raw/answers.json."""
    path = ctx.raw_dir / "answers.json"
    out = json.loads(path.read_text()) if path.exists() else {}
    for name, q in queries.items():
        out[name] = (cluster or ctx.cluster).scalar(
            "SELECT md5(coalesce(string_agg(r::text, E'\\n' ORDER BY r::text COLLATE \"C\"), '')) "
            f"FROM ({q}) r", dbname=dbname or ctx.dbname)
    out.update(values or {})
    path.write_text(json.dumps(out, indent=1, sort_keys=True) + "\n")
    return out


def probes(n: int, salt: int) -> list[tuple[int, int]]:
    """n fixed probe points, the same in every run."""
    def h(i, s):
        return (i * 2654435761 + s * 40503 + 12345) % 4294967296 / 4294967296
    return [(int(h(i, salt) * EXTENT), int(h(i, salt + 1) * EXTENT)) for i in range(1, n + 1)]


# ---------------------------------------------------------------------------
# G: storage
# ---------------------------------------------------------------------------

class GWorkload(Workload):
    suite = "G"
    data: list[str] = []

    def prepare(self, ctx: Context) -> None:
        load(ctx, self.data)

    def tables(self, ctx):
        return list(self.data)


class GPgbench(GWorkload):
    """A pgbench run of spatial scripts (scripts/) over the generated tables."""

    read_only = True

    def scripts(self, ctx: Context) -> list[tuple[str, int]]:
        raise NotImplementedError

    def defines(self, ctx: Context) -> dict[str, Any]:
        return {"extent": EXTENT, "srid": SRID}

    def warmup(self, ctx):
        if ctx.warmup_seconds > 0:
            run_pgbench(ctx, seconds=ctx.warmup_seconds, scripts=self.scripts(ctx),
                        defines=self.defines(ctx), tag="warmup", log_latency=False)

    def measure(self, ctx, window):
        res = run_pgbench(ctx, seconds=ctx.measure_seconds, scripts=self.scripts(ctx),
                          defines=self.defines(ctx), tag="measure", window=window,
                          transactions=ctx.params.get("transactions"))
        return Outcome(metrics=res.metrics(), transactions=res.transactions,
                       client_cpu_seconds=res.client_cpu_seconds, read_only=self.read_only)


class SingleStream(GWorkload):
    def client_counts(self, configured, variant):
        return [1]


@registry.workloads.register("G1")
class G1(SingleStream):
    """COPY of points and buildings, then GiST, SP-GiST and BRIN builds and
    a checkpoint, each timed; the indexes' sizes per access method."""

    id, title = "G1", "Bulk load and spatial index builds"
    data = ["gpts", "gbld"]

    def _file(self, ctx, t):
        return ctx.cluster.data_dir.parent / f"g1_{t}.copy"

    def prepare(self, ctx):
        # (generated in heap tables in public and written to files, so the
        # generation isn't measured)
        for t in self.data:
            columns, rows = TABLES[t]
            _sql(ctx, f"""CREATE TABLE public.{t}_src ({columns}) USING heap;
                INSERT INTO public.{t}_src {rows(ctx)};
                COPY public.{t}_src TO '{self._file(ctx, t)}';
                DROP TABLE public.{t}_src;
                CREATE TABLE {t} ({columns});""")

    def measure(self, ctx, window):
        steps = [("copy_seconds", "; ".join(f"COPY {t} FROM '{self._file(ctx, t)}'" for t in self.data)),
                 ("gist_seconds", "CREATE INDEX g1_gist ON gpts USING gist (g); "
                                  "CREATE INDEX g1_bld_gist ON gbld USING gist (g)"),
                 ("spgist_seconds", "CREATE INDEX g1_spgist ON gpts USING spgist (g)"),
                 ("brin_seconds", "CREATE INDEX g1_brin ON gpts USING brin (g)"),
                 ("checkpoint_seconds", "CHECKPOINT")]
        timings = {}
        with window:
            for name, sql in steps:
                t0 = time.monotonic()
                _sql(ctx, sql)
                timings[name] = time.monotonic() - t0
        for t in self.data:
            self._file(ctx, t).unlink(missing_ok=True)
        rows = size(ctx, "g_points") + size(ctx, "g_buildings")
        sizes = {f"{am}_index_bytes": float(_scalar(ctx, (
            "SELECT coalesce(sum(pg_relation_size(c.oid)), 0) FROM pg_class c JOIN pg_am a ON a.oid = c.relam "
            f"WHERE c.relname LIKE 'g1\\_%' AND a.amname = '{am}'")))
            for am in ("gist", "spgist", "brin")}
        total = sum(timings.values())
        return Outcome(metrics={**timings, **sizes, "total_seconds": total, "rows": rows,
                                "rows_per_second": rows / total},
                       transactions=rows, unit="row")

    def verify(self, ctx):
        answers(ctx, {t: digest_sql(t) for t in self.data})


@registry.workloads.register("G2")
class G2(GPgbench):
    """Random windows over the buildings, through the GiST index: bounding
    box overlap (&&), or ST_Intersects."""

    id, title = "G2", "Window queries"
    data = ["gbld"]

    def variants(self, params):
        return [(p, {"predicate": p}) for p in params.get("predicates", ["bbox", "intersects"])]

    def scripts(self, ctx):
        return [(f"g2_{ctx.params['predicate']}.sql", 1)]

    def defines(self, ctx):
        return {**super().defines(ctx), "w": int(ctx.params.get("window", 2000))}

    def verify(self, ctx):
        w = int(ctx.params.get("window", 2000))
        qs = {}
        for i, (x, y) in enumerate(probes(50, 21)):
            env = f"ST_MakeEnvelope({x}, {y}, {x + w}, {y + w}, {SRID})"
            pred = f"g && {env}" if ctx.params["predicate"] == "bbox" else f"ST_Intersects(g, {env})"
            qs[f"window{i}"] = f"SELECT id FROM gbld WHERE {pred}"
        answers(ctx, qs)


@registry.workloads.register("G3")
class G3(GPgbench):
    """The k nearest points to a random point: ORDER BY g <-> point LIMIT k."""

    id, title = "G3", "KNN"
    data = ["gpts"]

    def variants(self, params):
        return [(f"k{k}", {"k": int(k)}) for k in params.get("k", [1, 10, 100])]

    def scripts(self, ctx):
        return [("g3_knn.sql", 1)]

    def defines(self, ctx):
        return {**super().defines(ctx), "k": ctx.params["k"]}

    def verify(self, ctx):
        k = ctx.params["k"]
        answers(ctx, {f"knn{i}": (
            f"SELECT id, round((g <-> ST_SetSRID(ST_MakePoint({x}, {y}), {SRID}))::numeric, 6) FROM "
            f"(SELECT id, g FROM gpts ORDER BY g <-> ST_SetSRID(ST_MakePoint({x}, {y}), {SRID}), id LIMIT {k}) s")
            for i, (x, y) in enumerate(probes(20, 31))})


@registry.workloads.register("G4")
class G4(SingleStream):
    """Spatial joins, one stream: points in zones (counts per zone), and
    points in the large polygons by a nested loop that probes each polygon
    once per point (PostGIS's toast cache, hypothesis H3), with and without
    parallel workers."""

    id, title = "G4", "Spatial joins"
    data = ["gpts", "gzones", "glarge"]

    QUERIES = {
        "zones": "SELECT z.id, count(*) FROM gzones z JOIN gpts p ON ST_Contains(z.g, p.g) GROUP BY z.id",
        "large": "SELECT l.id, count(*) FROM glarge l JOIN gpts p ON ST_Intersects(l.g, p.g) GROUP BY l.id",
    }

    def variants(self, params):
        return [(f"par{w}", {"workers": int(w)}) for w in params.get("parallel", [0, 4])]

    def settings(self, ctx, name):
        s = [f"SET max_parallel_workers_per_gather = {ctx.params['workers']}"]
        if name == "large":
            # (each point probed against the polygon on the outer side)
            s += ["SET enable_hashjoin = off", "SET enable_mergejoin = off"]
        return s

    def measure(self, ctx, window):
        n = int(ctx.params.get("iterations", 5))
        metrics = {}
        with window:
            for name, q in self.QUERIES.items():
                stmts = self.settings(ctx, name) + [q] * n
                t = timed_session(ctx, stmts, f"g4-{name}")[len(stmts) - n:]
                metrics[f"{name}_ms"] = _median(t)
                metrics[f"{name}_p95_ms"] = _p95(t)
        return Outcome(metrics=metrics, transactions=n * len(self.QUERIES), unit="query",
                       read_only=True)

    def verify(self, ctx):
        answers(ctx, self.QUERIES)


@registry.workloads.register("G5")
class G5(GPgbench):
    """Moving objects: random fleet points moved (non-HOT updates, GiST
    maintenance), with inserts and deletes at fixed ratios."""

    id, title = "G5", "Moving-object updates"
    data = ["gfleet"]
    read_only = False
    SCRIPTS = [("g5_move.sql", 8), ("g5_insert.sql", 1), ("g5_delete.sql", 1)]

    def scripts(self, ctx):
        return self.SCRIPTS

    def defines(self, ctx):
        return {**super().defines(ctx), "n": size(ctx, "g_fleet")}

    def verify(self, ctx):
        # (what the run did is random; the fleet's points still all lie in
        # the extent, and the index agrees with a sequential scan)
        env = f"ST_MakeEnvelope(0, 0, {EXTENT}, {EXTENT}, {SRID})"
        answers(ctx, {}, {"fleet_in_extent": _scalar(ctx, f"SELECT bool_and(g && {env}) FROM gfleet"),
                          "index_agrees": _scalar(ctx, (
                              f"SELECT (SELECT count(*) FROM gfleet WHERE g && {env}) = (SELECT count(*) FROM gfleet)"))})


@registry.workloads.register("G6")
class G6(SingleStream):
    """Large polygons: a full read (vertex counts and areas), ST_Simplify,
    and a rewrite of one vertex of each, timed per statement."""

    id, title = "G6", "Large geometries"
    data = ["glarge"]

    def measure(self, ctx, window):
        n = size(ctx, "g_large")
        reps = int(ctx.params.get("iterations", 3))
        stmts = []
        for r in range(reps):
            stmts += ["SELECT sum(ST_NPoints(g)), round(sum(ST_Area(g))) FROM glarge",
                      "SELECT sum(ST_NPoints(ST_Simplify(g, 50))) FROM glarge",
                      # one vertex moved by a millimetre, in every polygon
                      f"UPDATE glarge SET g = ST_MakePolygon(ST_SetPoint(ST_ExteriorRing(g), {r + 1}, "
                      f"ST_Translate(ST_PointN(ST_ExteriorRing(g), {r + 2}), 0.001, 0)))"]
        with window:
            t = timed_session(ctx, stmts, "g6")
        rewrite = _median(t[2::3]) or 0.0
        return Outcome(metrics={"read_ms": _median(t[0::3]), "simplify_ms": _median(t[1::3]),
                                "rewrite_ms": rewrite, "rewrite_ms_per_polygon": rewrite / n},
                       transactions=reps * n, unit="polygon")

    def verify(self, ctx):
        answers(ctx, {"glarge": digest_sql("glarge"),
                      "simplified": "SELECT id, md5(ST_AsEWKB(ST_Simplify(g, 50))) FROM glarge"})


@registry.workloads.register("G7")
class G7(SingleStream):
    """ST_AsMVT tiles over a fixed tile list (zooms 4 to 7 of the extent):
    a tile server's index probe, clip and aggregate."""

    id, title = "G7", "Vector tiles"
    data = ["gbld", "gpts"]

    def tiles(self) -> list[tuple[int, int, int]]:
        return [(z, x * (2 ** z) // EXTENT, y * (2 ** z) // EXTENT)
                for z in (4, 5, 6, 7) for x, y in probes(8, 40 + z)]

    def tile_sql(self, z, x, y) -> str:
        env = f"ST_TileEnvelope({z}, {x}, {y}, ST_MakeEnvelope(0, 0, {EXTENT}, {EXTENT}, {SRID}))"
        layer = ("SELECT ST_AsMVT(t, '{name}') FROM (SELECT id, ST_AsMVTGeom(g, {env}) AS geom "
                 "FROM {table} WHERE g && {env} ORDER BY id) t")
        return (f"SELECT md5(coalesce(({layer.format(name='bld', table='gbld', env=env)}), '') || "
                f"coalesce(({layer.format(name='pts', table='gpts', env=env)}), ''))")

    def measure(self, ctx, window):
        tiles = self.tiles()
        with window:
            t = timed_session(ctx, [self.tile_sql(*tile) for tile in tiles], "g7")
        return Outcome(metrics={"tile_ms": _median(t), "tile_p95_ms": _p95(t)},
                       transactions=len(tiles), unit="tile", read_only=True)

    def verify(self, ctx):
        answers(ctx, {f"tile{z}_{x}_{y}": self.tile_sql(z, x, y) for z, x, y in self.tiles()})


@registry.workloads.register("G8")
class G8(GPgbench):
    """Geography: points within 50 km of a random point (ST_DWithin through
    the geography GiST index)."""

    id, title = "G8", "Geography"
    data = ["ggeog"]

    def scripts(self, ctx):
        return [("g8_dwithin.sql", 1)]

    def defines(self, ctx):
        return {"radius": int(ctx.params.get("radius", 50000))}

    def verify(self, ctx):
        r = int(ctx.params.get("radius", 50000))
        answers(ctx, {f"dwithin{i}": (f"SELECT id FROM ggeog WHERE ST_DWithin(g, ST_MakePoint("
                                      f"{x * 360 // EXTENT - 180}, {y * 170 // EXTENT - 85})::geography, {r})")
                      for i, (x, y) in enumerate(probes(20, 51))})


@registry.workloads.register("G9")
class G9(SingleStream):
    """ANALYZE of every generated table (PostGIS's own statistics, sampled
    over block-mapped pages), and the plans of window, KNN, join and
    geography queries, which must be heap's."""

    id, title = "G9", "ANALYZE and plans"
    data = ["gpts", "gbld", "gzones", "glarge", "ggeog"]

    PLANS = {
        "window": f"SELECT count(*) FROM gbld WHERE g && ST_MakeEnvelope(1000, 1000, 5000, 5000, {SRID})",
        "intersects": f"SELECT count(*) FROM gbld WHERE ST_Intersects(g, ST_MakeEnvelope(1000, 1000, 5000, 5000, {SRID}))",
        "knn": f"SELECT id FROM gpts ORDER BY g <-> ST_SetSRID(ST_MakePoint(500000, 500000), {SRID}) LIMIT 10",
        "zones": G4.QUERIES["zones"],
        "dwithin": "SELECT count(*) FROM ggeog WHERE ST_DWithin(g, ST_MakePoint(0, 0)::geography, 50000)",
    }

    def measure(self, ctx, window):
        stmts = [f"ANALYZE {t}" for t in self.data]
        with window:
            t = timed_session(ctx, stmts, "g9")
        return Outcome(metrics={"analyze_ms": sum(t), **{f"analyze_{n}_ms": x for n, x in zip(self.data, t)}},
                       transactions=len(stmts), unit="table")

    def verify(self, ctx):
        plans = {}
        for name, q in self.PLANS.items():
            plans[f"plan_{name}"] = ctx.cluster.sql(
                f"SET max_parallel_workers_per_gather = 0;\nEXPLAIN (COSTS OFF) {q}",
                dbname=ctx.dbname, options=ctx.session_options()).stdout.strip()
        answers(ctx, {"estimated_extent": "SELECT ST_EstimatedExtent('gpts', 'g')::text"}, plans)


# ---------------------------------------------------------------------------
# GV: version control
# ---------------------------------------------------------------------------

def _commit(msg: str) -> str:
    return f"SELECT dolt_commit('-Am', '{msg}')"


def _band(n: int, side: int = 0) -> str:
    """A predicate for n points (one band of ids per side)."""
    return f"id > {side * n} AND id <= {(side + 1) * n}"


class GVWorkload(Workload):
    """A spatial version-control workload: prolly only, against bounds.
    Each runs with nothing in the background, or with G5's moving objects
    ("G5"), as V's run with S1."""

    suite = "GV"
    data = ["gpts", "gbld", "glarge"]

    def check(self, ctx):
        if ctx.table_am not in targets.VERSIONED_HISTORY:
            raise NotImplementedByEngine(f"{ctx.table_am}: GV measures version control")

    def own_variants(self, params):
        return [("", {})]

    def variants(self, params):
        return [("-".join(x for x in (label, f"bg{bg}") if x), {**extra, "background": bg})
                for (label, extra), bg in product(self.own_variants(params),
                                                  params.get("background", ["none", "G5"]))]

    def client_counts(self, configured, variant):
        return configured if variant["background"] != "none" else configured[:1]

    def prepare(self, ctx):
        load(ctx, self.data)
        if ctx.params["background"] == "G5":
            load(ctx, ["gfleet"])
        timed_session(ctx, [_commit("gv data")], "prepare-commit")

    def background(self, ctx):
        if ctx.params["background"] == "G5":
            g5 = G5()
            return BackgroundPgbench(ctx, None, ctx.key.clients, "background",
                                     scripts=g5.scripts(ctx), defines=g5.defines(ctx))
        if ctx.params["background"] != "none":
            raise ValueError(f"unknown background workload {ctx.params['background']}")
        return nullcontext()

    def measure(self, ctx, window):
        with self.background(ctx) as bg:
            with window:
                outcome = self.operations(ctx)
        if bg is not None:
            outcome.metrics["background_tps"] = bg.tps()
        return outcome

    def operations(self, ctx) -> Outcome:
        raise NotImplementedError

    def tables(self, ctx):
        return list(self.data) + (["gfleet"] if ctx.params["background"] == "G5" else [])


@registry.workloads.register("GV1")
class GV1(GVWorkload):
    """A commit after N points moved (and one large polygon moved), N swept:
    the work should follow N, not the table (hypothesis H4)."""

    id, title = "GV1", "Commit cost against rows changed"

    def own_variants(self, params):
        return [(f"n{n}", {"changes": int(n)}) for n in params.get("changes", [1, 100, 10000])]

    def operations(self, ctx):
        n = ctx.params["changes"]
        if n > size(ctx, "g_points"):
            raise ValueError("GV1 changes exceed the points; refusing to clamp the workload")
        reps = int(ctx.params.get("iterations", 5))
        # (on a branch of its own, so a background workload's writes on main
        # aren't in its commits; the checkout is left out of the timings)
        stmts = ["SELECT dolt_checkout('-b', 'gv1_work')"]
        for i in range(reps):
            stmts += [f"UPDATE gpts SET g = ST_Translate(g, {i + 1}, 0) WHERE {_band(n)}",
                      f"UPDATE glarge SET g = ST_Translate(g, 0.001, 0) WHERE id = {i % size(ctx, 'g_large') + 1}",
                      _commit(f"gv1 {i}")]
        t = timed_session(ctx, stmts, "gv1")[1:]
        return Outcome(metrics={"edit_ms": _median(t[0::3]), "large_edit_ms": _median(t[1::3]),
                                "commit_ms": _median(t[2::3])},
                       transactions=reps, unit="commit")


@registry.workloads.register("GV2")
class GV2(GVWorkload):
    """Two branches moving bands of points, disjoint or the same, then a
    diff and a merge."""

    id, title = "GV2", "Diff and merge of regional edits"

    def own_variants(self, params):
        return [(f"n{n}-{o}", {"changes": int(n), "overlap": o})
                for n, o in product(params.get("changes", [1000]),
                                    params.get("overlap", ["disjoint", "overlapping"]))]

    def operations(self, ctx):
        n = ctx.params["changes"]
        if 2 * n > size(ctx, "g_points"):
            raise ValueError("GV2 changes exceed the points; refusing to clamp the workload")
        theirs = 0 if ctx.params["overlap"] == "overlapping" else 1
        stmts = ["SELECT dolt_branch('gv2_ours')", "SELECT dolt_branch('gv2_theirs')",
                 "SELECT dolt_checkout('gv2_theirs')",
                 f"UPDATE gpts SET g = ST_Translate(g, 0, 7) WHERE {_band(n, theirs)}",
                 _commit("gv2 theirs"),
                 "SELECT dolt_checkout('gv2_ours')",
                 f"UPDATE gpts SET g = ST_Translate(g, 7, 0) WHERE {_band(n)}",
                 _commit("gv2 ours"),
                 "SELECT count(*) FROM dolt_diff('gv2_ours', 'gv2_theirs', NULL::gpts)",
                 # (overlapping edits conflict: the merge keeps the conflicts)
                 "SET dolt.allow_commit_conflicts = on",
                 "SELECT dolt_merge('gv2_theirs')"]
        t = timed_session(ctx, stmts, "gv2")
        return Outcome(metrics={"diff_ms": t[8], "merge_ms": t[10]},
                       transactions=2, unit="operation")


def _timed(ctx: Context, sql: str, dbname: str) -> float:
    res = ctx.cluster.sql(f"\\timing on\n{sql};", dbname=dbname, options=ctx.session_options())
    return float(res.stdout.rsplit("Time: ", 1)[1].split(" ms")[0])


@registry.workloads.register("GV3")
class GV3(GVWorkload):
    """A push of the spatial database to a file remote, a clone of it into
    another database, a change pushed, and the clone's fetch of it."""

    id, title = "GV3", "Push, clone and fetch"

    def own_variants(self, params):
        return [(f"n{n}", {"changes": int(n)}) for n in params.get("changes", [1000])]

    def operations(self, ctx):
        n = ctx.params["changes"]
        remote = ctx.cluster.data_dir.parent / f"gv3-remote-{ctx.key.rep}"
        url = f"file://{remote}"
        push = timed_session(ctx, [f"SELECT dolt_remote('add', 'gv3', '{url}')",
                                   "SELECT dolt_push('gv3', 'main')"], "gv3-push")[1]
        clone_db = f"{ctx.dbname}_clone"
        ctx.cluster.sql(f"CREATE DATABASE {clone_db}")
        try:
            schema = targets.for_am(ctx.table_am).schema
            if schema:
                ctx.cluster.sql(f"ALTER DATABASE {clone_db} SET search_path = {schema}, public")
            clone = _timed(ctx, f"SELECT dolt_clone('{url}')", clone_db)
            if ctx.cluster.scalar(digest_sql("gpts"), dbname=clone_db) != _scalar(ctx, digest_sql("gpts")):
                raise RuntimeError("GV3's clone differs from its origin")
            change = timed_session(ctx, [f"UPDATE gpts SET g = ST_Translate(g, 3, 3) WHERE {_band(n)}",
                                         _commit("gv3 change"), "SELECT dolt_push('gv3', 'main')"],
                                   "gv3-change")[2]
            fetch = _timed(ctx, "SELECT dolt_fetch()", clone_db)
        finally:
            ctx.cluster.sql(f"DROP DATABASE IF EXISTS {clone_db} WITH (FORCE)")
        remote_bytes = sum(p.stat().st_size for p in remote.rglob("*") if p.is_file())
        return Outcome(metrics={"push_ms": push, "clone_ms": clone, "change_push_ms": change,
                                "fetch_ms": fetch, "remote_bytes": float(remote_bytes)},
                       transactions=4, unit="operation")


@registry.workloads.register("GV4")
class GV4(GVWorkload):
    """Growth over many small spatial commits (V7's method): the chunk
    store's bytes per commit, a point and one vertex of a large polygon
    moved in each."""

    id, title = "GV4", "Growth over small spatial commits"

    SPACE = ("SELECT coalesce(sum((pg_stat_file(d || '/' || f)).size), 0) FROM "
             "(SELECT 'base/' || oid AS d FROM pg_database "
             "WHERE datname = current_database()) db, pg_ls_dir(d) f "
             "WHERE f LIKE 'prolly\\_journal%' OR f LIKE 'prolly\\_table\\_%'")

    def operations(self, ctx):
        k = int(ctx.params.get("commits", 100))
        before = int(_scalar(ctx, self.SPACE))
        stmts = []
        for i in range(k):
            stmts += [f"UPDATE gpts SET g = ST_Translate(g, 1, 1) WHERE id = {i * 37 % size(ctx, 'g_points') + 1}",
                      f"UPDATE glarge SET g = ST_MakePolygon(ST_SetPoint(ST_ExteriorRing(g), {i % 50 + 1}, "
                      f"ST_Translate(ST_PointN(ST_ExteriorRing(g), {i % 50 + 2}), 0.001, 0))) "
                      f"WHERE id = {i % size(ctx, 'g_large') + 1}",
                      _commit(f"gv4 {i}")]
        t = timed_session(ctx, stmts, "gv4")
        grown = int(_scalar(ctx, self.SPACE)) - before
        large = int(_scalar(ctx, "SELECT avg(ST_MemSize(g))::bigint FROM glarge"))
        return Outcome(metrics={"commit_ms": _median(t[2::3]), "bytes_per_commit": grown / k,
                                "large_polygon_bytes": float(large),
                                "bytes_per_commit_per_polygon_byte": grown / k / large},
                       transactions=k, unit="commit")


@registry.workloads.register("GV5")
class GV5(GVWorkload):
    """A new branch's checkout and its first spatial queries (a window and
    a KNN, through the GiST indexes it shares), against the same queries
    run again."""

    id, title = "GV5", "Branch checkout and first query"

    def operations(self, ctx):
        reps = int(ctx.params.get("iterations", 5))
        q = [f"SELECT count(*) FROM gbld WHERE g && ST_MakeEnvelope(400000, 400000, 410000, 410000, {SRID})",
             f"SELECT id FROM gpts ORDER BY g <-> ST_SetSRID(ST_MakePoint(500000, 500000), {SRID}) LIMIT 10"]
        per = []
        for i in range(reps):
            per.append(timed_session(ctx, [f"SELECT dolt_branch('gv5_{i}')", f"SELECT dolt_checkout('gv5_{i}')",
                                           *q, *q], f"gv5-{i}"))
        return Outcome(metrics={"branch_ms": _median([t[0] for t in per]),
                                "checkout_ms": _median([t[1] for t in per]),
                                "first_query_ms": _median([t[2] + t[3] for t in per]),
                                "second_query_ms": _median([t[4] + t[5] for t in per])},
                       transactions=reps, unit="checkout")
