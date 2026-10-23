"""Post-measurement checks. Their work is never included in throughput."""
from __future__ import annotations

from pbench.core import process
from pbench.core.validation import atomic_json


def table_digests(ctx) -> dict:
    """Stream sorted record output into SHA-256, retaining NULLs and duplicates.

    PostgreSQL emits text in a fixed session format. Include column types;
    this is for before/after recovery of the same database, not an oracle for
    cross-engine floating-point expression evaluation. Record output uses each
    column's type output function rather than its potentially lossy JSON cast.
    COPY escapes embedded newlines/backslashes, preserving row framing.
    """
    result = {}
    for table in ctx.tables:
        types = ctx.cluster.scalar(
            "SELECT json_agg(a ORDER BY attnum) FROM (SELECT attnum, attname, "
            "atttypid, atttypmod FROM pg_attribute "
            f"WHERE attrelid='{table}'::regclass AND attnum>0 AND NOT attisdropped) a",
            dbname=ctx.dbname)
        options = {**ctx.session_options(), "timezone": "UTC", "datestyle": "ISO,YMD",
                   "extra_float_digits": "3", "intervalstyle": "iso_8601",
                   "bytea_output": "hex", "lc_monetary": "C"}
        # ROW(t.*) also avoids mistaking a column called t for the table alias.
        sql = (f"COPY (SELECT ROW(t.*)::text FROM {table} t "
               'ORDER BY ROW(t.*)::text COLLATE "C") TO STDOUT')
        digest = process.hash_output([ctx.cluster.bin("psql"), "-X", "-q", "-v", "ON_ERROR_STOP=1",
                                     "-d", ctx.dbname, "-c", sql], env=ctx.cluster.client_env(options))
        result[table] = {"columns": types, "row_encoding": "record-text-v1",
                         "rows_sha256": digest}
    return result


def verify(ctx, phase: str) -> None:
    """Cheap dataset and workload invariants, with explicit checked coverage."""
    checks = {}
    for table in ctx.tables:
        checks[table] = int(ctx.cluster.scalar(f"SELECT count(*) FROM {table}", dbname=ctx.dbname))
    wid = ctx.key.workload
    if wid in ("S1", "S2", "S3", "S6", "S8"):
        scale = int(ctx.size_params["pgbench_scale"])
        for table, count in (("pgbench_accounts", 100000 * scale),
                             ("pgbench_branches", scale), ("pgbench_tellers", 10 * scale)):
            if checks[table] != count:
                raise RuntimeError(f"{table}: expected {count} rows, found {checks[table]}")
        if wid in ("S1", "S6"):
            sums = [int(ctx.cluster.scalar(f"SELECT coalesce(sum({column}),0) FROM {table}", dbname=ctx.dbname))
                    for table, column in (("pgbench_accounts", "abalance"), ("pgbench_branches", "bbalance"),
                                          ("pgbench_tellers", "tbalance"), ("pgbench_history", "delta"))]
            if len(set(sums)) != 1:
                raise RuntimeError(f"pgbench balance/history invariant failed: {sums}")
            checks["balance_totals"] = sums
    if wid == "S4" and phase == "after":
        if checks["s4"] != int(ctx.size_params["s4_rows"]):
            raise RuntimeError("bulk load row count differs from requested count")
    if wid in ("S9", "S13"):
        if any(n == 0 for n in checks.values()):
            raise RuntimeError("TPC dataset contains an unexpectedly empty table")
        if wid == "S9":
            warehouses = int(ctx.size_params["tpcc_warehouses"])
            expected = {"warehouse": warehouses, "district": warehouses * 10,
                        "customer": warehouses * 30000, "stock": warehouses * 100000, "item": 100000}
            if any(checks[t] != n for t, n in expected.items()):
                raise RuntimeError("TPC-C scale/cardinality invariant failed")
            # TPC-C consistency condition 1: warehouse YTD equals its
            # districts' YTD; payments change both atomically.
            mismatch = ctx.cluster.scalar("""SELECT count(*) FROM warehouse w WHERE w_ytd <>
                (SELECT sum(d_ytd) FROM district d WHERE d.d_w_id=w.w_id)""", dbname=ctx.dbname)
            if mismatch != "0":
                raise RuntimeError("TPC-C warehouse/district balance invariant failed")
            checks["warehouse_district_balance"] = "equal"
        else:
            if checks["region"] != 5 or checks["nation"] != 25:
                raise RuntimeError("TPC-H region/nation cardinalities are incorrect")
    atomic_json(ctx.raw_dir / f"correctness-{phase}.json", {
        "checks": checks, "scope": "cardinality and named invariants; query answers require differential correctness"})
