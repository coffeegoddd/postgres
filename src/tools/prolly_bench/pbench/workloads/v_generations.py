"""V11: generational chunk-store files under steady churn.

A table of the vc_bench shape (an int64 key, an int64 revision and 1,024
bytes of row-specific base64 payload a row) is loaded, committed and
collected, so the base load sits in a generation of its own.  Then each
cycle:

  1. updates `churn` rows spread evenly over the keys, a different set
     each cycle (the revision set to the cycle's number);
  2. commits, dated a day after the previous commit;
  3. runs a default collection, which seals a new generation;
  4. severs the history older than the newest `window_cycles` commits,
     in delete mode (dolt_history_drop());
  5. retires what the reclamation chooses (the generations thought at
     least half garbage), timing it, and counting its WAL;
  6. vacuums the table (outside every timing), then reads the store's
     sizes, its files and their sections, pg_stat_prolly and a batch of
     point reads of the oldest commit the window keeps.

`warm_cycles` cycles come first, then `cycles` measured ones at `churn`,
then `sweep_cycles` cycles at each of the `sweep` churns.  After them the
table files are conjoined (to `conjoin_files`) and the point reads taken
again, and a full collection gives what the retires left as garbage.

With `standby`, a streaming standby made by a base backup after the load
replays everything; each retire's replay time is how long after the
retire returned the standby had replayed its WAL.  With `retire` false
(a build without retires), cycles stop after the sever: the default
collection's times can then be compared with a build that doesn't record
sections.

Every cycle's numbers are in raw/v11-cycles.jsonl; the cell's metrics are
their summaries.  The chunk-store functions it drives are test_prolly's
(test_chunk_gc, test_chunk_retire, test_chunk_retire_choose,
test_chunk_generations, test_chunk_conjoin): build with
build_postgres.py --with-test-prolly.
"""

from __future__ import annotations

import json
import os
import re
import statistics
import struct
import subprocess
import time
from pathlib import Path
from typing import Any, Optional

from pbench.core import process, registry
from pbench.core.interfaces import Context, Outcome, Window
from pbench.core.sqlerr import raise_for_sql_error
from pbench.workloads.v_suite import SESSIONS_EXITED, VWorkload

_TIME_RE = re.compile(r"^Time: ([\d.]+) ms.*$", re.M)

# The first commit's date; cycle i commits a day after cycle i - 1.  Old
# enough that every commit is past the default history window, so a sever
# may cut it.
EPOCH_DAY = "2020-01-01 00:00+00"

# The table file format's footer: its last 48 bytes, the section's length
# a little-endian uint32 at 20 (tablefile_basic.c).
FOOTER_SIZE = 48
FOOTER_SECTION_LEN = 20

PAYLOAD_SQL = """
CREATE FUNCTION public.v11_payload(id bigint) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT translate(encode((SELECT string_agg(sha512(convert_to(k || ':' || id, 'UTF8')), ''::bytea)
							 FROM generate_series(1, 12) k), 'base64'), E'\\n', '')
$$;
"""


def day(n: int) -> str:
    return f"(timestamptz '{EPOCH_DAY}' + make_interval(days => {n}))"


def percentile(xs: list[float], p: float) -> Optional[float]:
    if not xs:
        return None
    xs = sorted(xs)
    k = (len(xs) - 1) * p / 100
    lo = int(k)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def median(xs: list[float]) -> Optional[float]:
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def slope(xs: list[float], ys: list[float]) -> Optional[float]:
    """Least-squares slope of ys against xs."""
    if len(xs) < 2:
        return None
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    den = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den if den else None


def churn_ids(rows: int, churn: int, cycle: int) -> tuple[int, int]:
    """The keys cycle updates: first, first + stride, ... up to rows, churn
    of them, a different offset each cycle (first, stride)."""
    if churn < 1 or churn > rows:
        raise ValueError(f"V11 churn {churn} must be between 1 and the table's {rows} rows")
    stride = rows // churn
    return 1 + cycle % stride, stride


def timed(cluster, dbname: str, statements: list[str]) -> list[tuple[list[str], float]]:
    """Run statements in one session: each one's output rows and time (ms)."""
    script = "\\timing on\n" + "\n".join(s.rstrip().rstrip(";") + ";" for s in statements) + "\n"
    res = cluster.sql(script, dbname=dbname, check=False)
    raise_for_sql_error(res, script)
    out, parts, pos = [], [], 0
    for m in _TIME_RE.finditer(res.stdout):
        rows = [r for r in res.stdout[pos:m.start()].splitlines() if r.strip()]
        out.append((rows, float(m.group(1))))
        pos = m.end()
    if len(out) != len(statements):
        raise RuntimeError(f"expected {len(statements)} timings, got {len(out)}: {res.stdout[-2000:]}")
    return out


@registry.workloads.register("V11")
class V11(VWorkload):
    id, title = "V11", "Generational retires under steady churn"

    def own_variants(self, params):
        return [("retire" if params.get("retire", True) else "noretire", {})]

    def variants(self, params):
        # (no background: the retires' times are the measurement)
        return [(label, {**extra, "background": "none"}) for label, extra in self.own_variants(params)]

    def tables(self, ctx):
        return ["vc_rows"]

    # -- set up -----------------------------------------------------------

    def prepare(self, ctx: Context) -> None:
        n = self.rows(ctx)
        p = ctx.params
        self.retire = bool(p.get("retire", True))
        sql = ctx.cluster.sql
        sql("CREATE EXTENSION test_prolly SCHEMA public;\n" + PAYLOAD_SQL +
            "CREATE TABLE vc_rows (id bigint PRIMARY KEY, rev bigint NOT NULL, payload text NOT NULL);",
            dbname=ctx.dbname, options=ctx.session_options())
        batch = int(p.get("load_batch", 100000))
        t0 = time.monotonic()
        stmts = [f"INSERT INTO vc_rows SELECT g, 0, public.v11_payload(g) "
                 f"FROM generate_series({lo}, {min(lo + batch - 1, n)}) g"
                 for lo in range(1, n + 1, batch)]
        sql(";\n".join(stmts) + ";", dbname=ctx.dbname, options=ctx.session_options())
        load_s = time.monotonic() - t0
        t0 = time.monotonic()
        sql(f"SELECT dolt_commit('-Am', 'base', '--date', {day(0)}::text)", dbname=ctx.dbname)
        commit_s = time.monotonic() - t0
        t0 = time.monotonic()
        sql("SELECT test_chunk_gc(false)", dbname=ctx.dbname)
        gc_s = time.monotonic() - t0
        sql("VACUUM (ANALYZE) vc_rows", dbname=ctx.dbname)
        sql("CHECKPOINT")
        self.dboid = ctx.cluster.scalar("SELECT oid FROM pg_database WHERE datname = current_database()",
                                        dbname=ctx.dbname)
        self.prep = {"load_s": load_s, "base_commit_s": commit_s, "base_gc_s": gc_s,
                     "rows": n, **self.store(ctx)}
        self.standby = self.make_standby(ctx) if p.get("standby", False) else None
        (ctx.raw_dir / "v11-prepare.json").write_text(json.dumps(self.prep, indent=1) + "\n")

    def make_standby(self, ctx: Context):
        """A streaming standby from a base backup of the primary."""
        from pbench.clusters.local import LocalCluster
        from pbench.core.runner import free_port
        primary = ctx.cluster
        sb = LocalCluster(primary.bin_dir, primary.base.parent, free_port(), primary.base.name + "-standby")
        ctx.peers.append(sb)
        sb.base.mkdir(parents=True, exist_ok=True)
        t0 = time.monotonic()
        r = process.run([primary.bin("pg_basebackup"), "-h", str(primary.sock_dir), "-p",
                         str(primary.port), "-U", "postgres", "-D", str(sb.data_dir), "-X", "stream",
                         "-R", "-c", "fast"], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"pg_basebackup failed: {r.stderr[-2000:]}")
        self.prep["basebackup_s"] = time.monotonic() - t0
        with open(sb.data_dir / "postgresql.conf", "a") as f:
            f.write(f"\n# --- V11 standby ---\nport = {sb.port}\n"
                    f"unix_socket_directories = '{sb.sock_dir}'\nhot_standby = on\n"
                    "max_standby_streaming_delay = 30s\n")
        sb.start()
        sb.wait_ready()
        self.wait_replayed(ctx, sb, self.lsn(ctx))
        return sb

    # -- reading the store --------------------------------------------------

    def lsn(self, ctx) -> str:
        return ctx.cluster.scalar("SELECT pg_current_wal_insert_lsn()")

    def wait_replayed(self, ctx, sb, lsn: str, timeout: float = 600.0) -> float:
        """Seconds until the standby had replayed past lsn."""
        t0 = time.monotonic()
        while True:
            r = sb.sql(f"SELECT pg_last_wal_replay_lsn() >= '{lsn}'::pg_lsn", check=False)
            if r.ok and r.stdout.strip() == "t":
                return time.monotonic() - t0
            if time.monotonic() - t0 > timeout:
                raise RuntimeError(f"the standby didn't replay to {lsn} in {timeout:.0f} s")
            time.sleep(0.002)

    def store(self, ctx) -> dict[str, Any]:
        """The chunk store's sizes and files, its sections' bytes read from
        the files' footers, and the data directory's bytes."""
        s = ctx.cluster.scalar(
            "SELECT json_build_object('journal_segments', journal_segments, 'journal_bytes', journal_bytes,"
            " 'table_files', table_files, 'table_file_bytes', table_file_bytes,"
            " 'settled_files', settled_files, 'settled_bytes', settled_bytes) FROM prolly_store_sizes()",
            dbname=ctx.dbname)
        out = json.loads(s)
        out["store_bytes"] = out["journal_bytes"] + out["table_file_bytes"]
        dbdir = Path(ctx.cluster.data_dir) / "base" / ctx.cluster.scalar(
            "SELECT oid FROM pg_database WHERE datname = current_database()", dbname=ctx.dbname)
        section = 0
        files = 0
        for f in dbdir.iterdir():
            if not f.name.startswith("prolly_table_"):
                continue
            try:
                with open(f, "rb") as fh:
                    fh.seek(-FOOTER_SIZE, os.SEEK_END)
                    foot = fh.read(FOOTER_SIZE)
            except OSError:
                continue
            if len(foot) == FOOTER_SIZE and foot[-8:] == b"PRLYTBL1":
                section += struct.unpack_from("<I", foot, FOOTER_SECTION_LEN)[0]
                files += 1
        out["section_bytes_on_disk"] = section
        out["table_files_on_disk"] = files
        total = 0
        for root, _, names in os.walk(ctx.cluster.data_dir):
            for name in names:
                try:
                    total += os.lstat(os.path.join(root, name)).st_size
                except OSError:
                    pass
        out["data_dir_bytes"] = total
        out["relation_bytes"] = int(ctx.cluster.scalar(
            "SELECT pg_total_relation_size('vc_rows')", dbname=ctx.dbname))
        return out

    def generations(self, ctx) -> list[dict[str, Any]]:
        if not self.retire:
            return []
        s = ctx.cluster.scalar(
            "SELECT coalesce(json_agg(g ORDER BY epoch_lo, name), '[]') FROM test_chunk_generations() g",
            dbname=ctx.dbname)
        return json.loads(s)

    def counters(self, ctx) -> dict[str, int]:
        out = ctx.cluster.sql(SESSIONS_EXITED + "\nSELECT pg_stat_force_next_flush();\n"
                              "SELECT name || '=' || value FROM pg_stat_prolly",
                              dbname=ctx.dbname).stdout.split()
        return {k: int(v) for k, v in (line.split("=", 1) for line in out if "=" in line)}

    def point_reads(self, ctx, commit: str, reads: int, seed: int) -> dict[str, Any]:
        """reads point reads of commit by key, in one new session: each
        one's time, and the chunk-store reads they took."""
        n = self.rows(ctx)
        # (distinct keys, spread, a different set each call)
        keys = [1 + (seed * 7919 + i * (n // reads + 1)) % n for i in range(reads)]
        before = self.counters(ctx)
        # (a snapshot mount of the commit, which seeks its tree by the key;
        # mounted and unmounted around the reads, so its pin holds nothing
        # past them)
        ctx.cluster.sql(f"SELECT prolly_mount('{commit}', 'versioned', 'v11_at', ARRAY['vc_rows'])",
                        dbname=ctx.dbname)
        try:
            got = timed(ctx.cluster, ctx.dbname,
                        [f"SELECT id FROM v11_at.vc_rows WHERE id = {k}" for k in keys])
        finally:
            ctx.cluster.sql("SELECT prolly_unmount('v11_at')", dbname=ctx.dbname)
        after = self.counters(ctx)
        for (rows, _), k in zip(got, keys):
            if rows != [str(k)]:
                raise RuntimeError(f"point read of {k} at {commit} returned {rows}")
        ms = [t for _, t in got]
        d = {k: after.get(k, 0) - before.get(k, 0)
             for k in ("chunk_file_reads", "node_cache_misses", "node_cache_hits", "nodes_read")}
        return {"read_ms_p50": percentile(ms, 50), "read_ms_p95": percentile(ms, 95),
                "read_ms_mean": statistics.fmean(ms),
                **{f"read_{k}_per_read": v / reads for k, v in d.items()}}

    # -- the cycles -----------------------------------------------------------

    def cycle(self, ctx, i: int, churn: int, phase: str) -> dict[str, Any]:
        p = ctx.params
        n = self.rows(ctx)
        db = ctx.dbname
        first, stride = churn_ids(n, churn, i)
        rec: dict[str, Any] = {"cycle": i, "phase": phase, "churn": churn}
        c0 = self.counters(ctx)
        upd, com, gc = timed(ctx.cluster, db, [
            f"UPDATE vc_rows r SET rev = {i} FROM generate_series({first}, {n}, {stride}) g "
            f"WHERE r.id = g AND g < {first} + {stride} * {churn}",
            f"SELECT dolt_commit('-am', 'c{i}', '--date', {day(i)}::text)",
            "SELECT kept || ' ' || freed || ' ' || bytes_freed || ' ' || window_usec FROM test_chunk_gc(false)"])
        rec.update(update_ms=upd[1], commit_ms=com[1], default_gc_ms=gc[1])
        k, f, b, w = (int(x) for x in gc[0][0].split())
        rec.update(default_gc_kept=k, default_gc_freed=f, default_gc_bytes_freed=b, default_gc_window_us=w)
        c1 = self.counters(ctx)
        rec["default_gc_table_file_bytes_written"] = (c1.get("table_file_bytes_written", 0)
                                                      - c0.get("table_file_bytes_written", 0))
        rec["cycle_chunk_bytes_appended"] = (c1.get("chunk_bytes_appended", 0)
                                             - c0.get("chunk_bytes_appended", 0))
        gens = self.generations(ctx)
        if gens:
            newest = max(g["epoch_hi"] for g in gens)
            rec["new_file_xrefs"] = sum(g["xrefs"] for g in gens if g["epoch_lo"] == newest)
            rec["new_file_chunks"] = sum(g["chunks"] for g in gens if g["epoch_lo"] == newest)
            rec["generation"] = newest
        keep = int(p.get("window_cycles", 3))
        (drop,) = timed(ctx.cluster, db, [
            f"SELECT coalesce(commits, 0) || ' ' || coalesce(chunks, 0) || ' ' || coalesce(bytes, 0) "
            f"FROM dolt_history_drop({day(i - keep + 1)})"])
        rec["sever_ms"] = drop[1]
        rec["sever_commits"], rec["sever_chunks"], rec["sever_bytes"] = (int(x) for x in drop[0][0].split())
        if self.retire:
            rec.update(self.retire_step(ctx, gens_before=self.generations(ctx)))
        ctx.cluster.sql("VACUUM vc_rows", dbname=db)
        c2 = self.counters(ctx)
        rec["counters"] = {k: c2[k] - c0.get(k, 0) for k in c2 if c2[k] != c0.get(k, 0)}
        rec.update(self.store(ctx))
        # (the oldest commit the window keeps: its tree spans the most
        # generations)
        oldest = ctx.cluster.scalar(f"SELECT dolt_hashof('main~{min(keep - 1, i)}')", dbname=db)
        rec["read_commit"] = oldest
        rec.update(self.point_reads(ctx, oldest, int(p.get("reads", 200)), i))
        rec["loadavg"] = os.getloadavg()
        return rec

    def retire_step(self, ctx, gens_before) -> dict[str, Any]:
        db = ctx.dbname
        out: dict[str, Any] = {}
        lo_hi = ctx.cluster.scalar(
            "SELECT coalesce(lo::text, '') || ' ' || coalesce(hi::text, '') FROM test_chunk_retire_choose()",
            dbname=db).split()
        if len(lo_hi) != 2:
            out["retired"] = False
            return out
        lo, hi = int(lo_hi[0]), int(lo_hi[1])
        r_files = [g for g in gens_before if g["settled"] and lo <= g["epoch_lo"] and g["epoch_hi"] <= hi]
        out.update(retired=True, retire_lo=lo, retire_hi=hi, retire_files=len(r_files),
                   retire_r_chunks=sum(g["chunks"] for g in r_files),
                   retire_r_garbage_est=sum(g["garbage"] for g in r_files),
                   retire_inbound_xrefs=sum(g["xrefs"] for g in gens_before if g["epoch_lo"] > hi))
        names = {g["name"] for g in r_files}
        sizes = json.loads(ctx.cluster.scalar(
            "SELECT coalesce(json_object_agg(name, bytes), '{}') FROM test_chunk_table_files()", dbname=db))
        out["retire_r_bytes"] = sum(v for k, v in sizes.items() if k in names)
        if self.standby is not None:
            self.wait_replayed(ctx, self.standby, self.lsn(ctx))
        lsn0 = self.lsn(ctx)
        ((rows, ms),) = timed(ctx.cluster, db, [
            f"SELECT kept || ' ' || freed || ' ' || bytes_freed FROM test_chunk_retire({lo}, {hi})"])
        lsn1 = self.lsn(ctx)
        out["retire_ms"] = ms
        out["retire_kept"], out["retire_freed"], out["retire_bytes_freed"] = (int(x) for x in rows[0].split())
        out["retire_wal_bytes"] = int(ctx.cluster.scalar(f"SELECT pg_wal_lsn_diff('{lsn1}', '{lsn0}')"))
        if self.standby is not None:
            out["standby_replay_ms"] = 1000 * self.wait_replayed(ctx, self.standby, lsn1)
        after = {g["name"] for g in self.generations(ctx)}
        out["retire_files_dropped_or_rewritten"] = len(names - after)
        return out

    def measure(self, ctx: Context, window: Window) -> Outcome:
        p = ctx.params
        churn = int(p.get("churn", 10000))
        warm, cycles = int(p.get("warm_cycles", 10)), int(p.get("cycles", 10))
        sweep = [int(x) for x in p.get("sweep", [])]
        sweep_cycles = int(p.get("sweep_cycles", 3))
        log = (ctx.raw_dir / "v11-cycles.jsonl").open("w")
        recs: list[dict[str, Any]] = []

        def run(i, k, phase):
            rec = self.cycle(ctx, i, k, phase)
            recs.append(rec)
            log.write(json.dumps(rec) + "\n")
            log.flush()

        i = 0
        with window:
            for _ in range(warm):
                i += 1
                run(i, churn, "warm")
            for _ in range(cycles):
                i += 1
                run(i, churn, "steady")
            for k in sweep:
                for _ in range(sweep_cycles):
                    i += 1
                    run(i, k, f"sweep{k}")
            end = self.finish(ctx, recs)
        log.close()
        (ctx.raw_dir / "v11-end.json").write_text(json.dumps(end, indent=1) + "\n")
        return Outcome(metrics=self.summary(recs, end), transactions=len(recs), unit="cycle",
                       notes=[f"V11: {len(recs)} cycles; per-cycle numbers in v11-cycles.jsonl"])

    def finish(self, ctx, recs) -> dict[str, Any]:
        """After the cycles: a full collection on that state, every file
        with garbage rewritten (so what it frees is all the garbage the
        retires left); then point reads, a conjoin of the table files, and
        point reads again."""
        p = ctx.params
        db = ctx.dbname
        end: dict[str, Any] = {}
        if p.get("full", True):
            before = self.store(ctx)
            (_, (rows, ms)) = timed(ctx.cluster, db, [
                "SET prolly.gc_rewrite_below = 2147483647",
                "SELECT chunks_kept || ' ' || chunks_freed || ' ' || bytes_freed FROM prolly_gc(true)"])
            kept, freed, bytes_freed = (int(x) for x in rows[0].split())
            after = self.store(ctx)
            end.update(full_gc_ms=ms, full_gc_kept=kept, full_gc_freed=freed,
                       full_gc_bytes_freed=bytes_freed, before_full=before, after_full=after)
        files = int(p.get("conjoin_files", 0))
        if files and recs:
            commit = recs[-1]["read_commit"]
            end["before_conjoin"] = self.store(ctx)
            end["reads_before_conjoin"] = self.point_reads(ctx, commit, int(p.get("reads", 200)), 10007)
            ((_, ms),) = timed(ctx.cluster, db, [f"SELECT test_chunk_conjoin({files})"])
            end["conjoin_ms"] = ms
            end["after_conjoin"] = self.store(ctx)
            end["reads_after_conjoin"] = self.point_reads(ctx, commit, int(p.get("reads", 200)), 10007)
        if p.get("verify", True) and self.retire:
            ((rows, ms),) = timed(ctx.cluster, db, ["SELECT test_chunk_verify_generations()"])
            end["verified_chunks"], end["verify_ms"] = int(rows[0]), ms
        end["loadavg"] = os.getloadavg()
        return end

    @staticmethod
    def summary(recs, end) -> dict[str, Optional[float]]:
        m: dict[str, Optional[float]] = {}
        steady = [r for r in recs if r["phase"] == "steady"]
        ret = [r for r in steady if r.get("retired")]
        m["steady_cycles"] = len(steady)
        m["steady_retires"] = len(ret)
        for name in ("retire_ms", "retire_r_bytes", "retire_bytes_freed", "retire_wal_bytes",
                     "standby_replay_ms", "retire_files"):
            m[f"steady_{name}_median"] = median([r.get(name) for r in ret])
        m["steady_retire_ms_max"] = max((r["retire_ms"] for r in ret), default=None)
        for name in ("default_gc_ms", "commit_ms", "update_ms", "sever_ms"):
            m[f"steady_{name}_median"] = median([r[name] for r in steady])
        for k in sorted({r["churn"] for r in recs if r["phase"].startswith("sweep")} |
                        ({steady[0]["churn"]} if steady else set())):
            rs = [r for r in recs if r["churn"] == k and r["phase"] != "warm" and r.get("retired")]
            m[f"churn{k}_retire_ms_median"] = median([r["retire_ms"] for r in rs])
            m[f"churn{k}_retire_r_bytes_median"] = median([r["retire_r_bytes"] for r in rs])
        written = sum(r["default_gc_table_file_bytes_written"] for r in steady)
        section = [r.get("new_file_xrefs") for r in steady if r.get("new_file_xrefs") is not None]
        m["steady_section_entries_per_cycle_median"] = median(section)
        if steady:
            sec = [b - a for a, b in zip([r["section_bytes_on_disk"] for r in recs[:-1]],
                                         [r["section_bytes_on_disk"] for r in recs[1:]])]
            m["store_bytes_first_steady"] = steady[0]["store_bytes"]
            m["store_bytes_last_steady"] = steady[-1]["store_bytes"]
            xs = [r["cycle"] for r in steady]
            m["store_bytes_slope_per_cycle"] = slope(xs, [r["store_bytes"] for r in steady])
            m["data_dir_bytes_slope_per_cycle"] = slope(xs, [r["data_dir_bytes"] for r in steady])
            m["table_files_first_steady"] = steady[0]["table_files"]
            m["table_files_last_steady"] = steady[-1]["table_files"]
            m["default_gc_bytes_written_steady"] = written
            m["section_bytes_last"] = recs[-1]["section_bytes_on_disk"]
            for q in ("read_ms_p50", "read_ms_p95", "read_chunk_file_reads_per_read"):
                m[f"first_steady_{q}"] = steady[0][q]
                m[f"last_steady_{q}"] = steady[-1][q]
        for side in ("before", "after"):
            r = end.get(f"reads_{side}_conjoin")
            if r:
                m[f"{side}_conjoin_read_ms_p50"] = r["read_ms_p50"]
                m[f"{side}_conjoin_chunk_file_reads_per_read"] = r["read_chunk_file_reads_per_read"]
        if "after_conjoin" in end:
            m["conjoin_ms"] = end["conjoin_ms"]
            m["files_before_conjoin"] = end["before_conjoin"]["table_files"]
            m["files_after_conjoin"] = end["after_conjoin"]["table_files"]
        if "full_gc_ms" in end:
            m["full_gc_ms"] = end["full_gc_ms"]
            m["full_gc_bytes_freed"] = end["full_gc_bytes_freed"]
            m["store_bytes_before_full"] = end["before_full"]["store_bytes"]
            m["residual_garbage_ratio"] = (end["full_gc_bytes_freed"] / end["before_full"]["store_bytes"]
                                           if end["before_full"]["store_bytes"] else None)
            m["retired_bytes_freed_total"] = sum(r.get("retire_bytes_freed", 0) for r in recs)
        return m
