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

Warm-up cycles come first, enough that `warm_severs` severs (10) have
cut history before the measured phase (a sever cuts nothing until the
window is full, at cycle `window_cycles`); then `cycles` measured ones at
`churn`, then `sweep_cycles` cycles at each of the `sweep` churns.  After
them, on the store as the cycles left it, point reads are taken, the
table files are conjoined (to `conjoin_files`) and the point reads taken
again; last, a full collection gives what the retires left as garbage.

With `standby`, a streaming standby made by a base backup after the load
replays everything.  One session on it, kept open, polls its replay LSN
and its clock all through each retire; a retire's replay time is the
first poll that saw its WAL replayed, less the retire's end on the
primary's clock (the same host's), and its resolution the gap between
that poll and the one before.  With `retire` false, cycles stop after the
sever: so it runs on a build without retires, and on one with them, it
gives the default collection's times at the same file count as such a
build (a retire adds a file a cycle, the young copy), for comparing with
and without recording sections.

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
import threading
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

# The chunk store's own files in a database's directory: its table files,
# its journal's segments (prolly_journal, prolly_journal_<id>), and the
# manifest and the journal's index.  The first two are what
# prolly_store_sizes() counts.
STORE_FILE_PREFIXES = ("prolly_table_", "prolly_journal")
STORE_META_PREFIXES = ("prolly_manifest", "prolly_index")

# A WAL page's header (short, and long at a segment's start): an insert
# LSN at a page boundary is given past it, where no record ends.
XLOG_SHORT_PHD = 24
XLOG_LONG_PHD = 40

STANDBY_POLL_SQL = ("SELECT extract(epoch FROM clock_timestamp()) || ' ' || "
                    "coalesce(pg_last_wal_replay_lsn(), '0/0'::pg_lsn);\n")

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


def warm_cycles(params) -> int:
    """The warm-up cycles: warm_cycles if given, else enough for warm_severs
    severs that cut history (the first is at cycle window_cycles)."""
    if "warm_cycles" in params:
        return int(params["warm_cycles"])
    return int(params.get("window_cycles", 3)) - 1 + int(params.get("warm_severs", 10))


def lsn_int(lsn: str) -> int:
    hi, lo = lsn.strip().split("/")
    return (int(hi, 16) << 32) | int(lo, 16)


def replay_target(lsn: int, page: int = 8192, segment: int = 16 << 20) -> int:
    """What a standby's replay LSN reaches once it has replayed to the
    primary's insert LSN lsn.  At a page boundary the insert LSN is past
    the next page's header, but the last record ends at the boundary,
    which is where replay stops until more WAL comes."""
    if lsn % segment == XLOG_LONG_PHD:
        return lsn - XLOG_LONG_PHD
    if lsn % page == XLOG_SHORT_PHD:
        return lsn - XLOG_SHORT_PHD
    return lsn


def store_files(dbdir: Path) -> dict[str, Any]:
    """The chunk store's files as the filesystem has them: the bytes of its
    table files and journal segments (what the manifest's sizes should add
    up to), of its manifest and index, each table file's size and section
    length (read from its footer), and how many journal segments."""
    files_bytes = meta_bytes = segments = 0
    sections: dict[str, int] = {}
    tables: dict[str, int] = {}
    for f in dbdir.iterdir():
        name = f.name
        if name.startswith(STORE_META_PREFIXES):
            try:
                meta_bytes += f.lstat().st_size
            except OSError:
                pass
            continue
        if not name.startswith(STORE_FILE_PREFIXES):
            continue
        try:
            size = f.lstat().st_size
        except OSError:
            continue
        files_bytes += size
        if not name.startswith("prolly_table_"):
            segments += 1
            continue
        tables[name] = size
        try:
            with open(f, "rb") as fh:
                fh.seek(-FOOTER_SIZE, os.SEEK_END)
                foot = fh.read(FOOTER_SIZE)
        except OSError:
            continue
        if len(foot) == FOOTER_SIZE and foot[-8:] == b"PRLYTBL1":
            sections[name] = struct.unpack_from("<I", foot, FOOTER_SECTION_LEN)[0]
    return {"files_bytes": files_bytes, "meta_bytes": meta_bytes, "sections": sections,
            "tables": tables, "segments": segments}


def dir_bytes(top: Path, skip: tuple[str, ...] = ()) -> int:
    """The apparent bytes of top's files, less the top-level entries skip."""
    total = 0
    for root, dirs, names in os.walk(top):
        if Path(root) == Path(top):
            dirs[:] = [d for d in dirs if d not in skip]
            names = [n for n in names if n not in skip]
        for name in names:
            try:
                total += os.lstat(os.path.join(root, name)).st_size
            except OSError:
                pass
    return total


class StandbySession:
    """One session on the standby, kept open: a poll is one query on it, a
    round trip on an open connection rather than a psql start."""

    def __init__(self, sb):
        self.proc = subprocess.Popen(
            [sb.bin("psql"), "-X", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=1", "-d", "postgres"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, bufsize=1, env=sb.client_env())

    def poll(self) -> tuple[float, int]:
        """The standby's clock (epoch seconds) and replay LSN."""
        self.proc.stdin.write(STANDBY_POLL_SQL)
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError(f"the standby's poll session exited ({self.proc.poll()})")
        ts, lsn = line.split()
        return float(ts), lsn_int(lsn)

    def close(self) -> None:
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=10)
        except (OSError, subprocess.TimeoutExpired):
            self.proc.kill()
            self.proc.wait()
        self.proc.stdout.close()


class ReplayWatch:
    """Polls the standby from before a retire until it has replayed past a
    target given after it: the first poll that saw the target replayed,
    taken before the target was known or after, and the poll before it
    (the replay was between them)."""

    def __init__(self, poll, interval_s: float = 0.0005, timeout_s: float = 600.0):
        self.poll, self.interval, self.timeout = poll, interval_s, timeout_s
        self.target: Optional[int] = None
        self.polls: list[tuple[float, int]] = []
        self.hit: Optional[tuple[Optional[float], float]] = None
        self.error: Optional[BaseException] = None
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> "ReplayWatch":
        self.thread.start()
        return self

    @staticmethod
    def first_past(polls: list[tuple[float, int]], target: int) -> Optional[tuple[Optional[float], float]]:
        """The first of polls (time, replay LSN) at or past target, and the
        time of the one before (None: none before)."""
        for k, (ts, lsn) in enumerate(polls):
            if lsn >= target:
                return (polls[k - 1][0] if k else None), ts
        return None

    def _run(self) -> None:
        t0 = time.monotonic()
        try:
            while True:
                self.polls.append(self.poll())
                # (every poll is kept: the standby may have replayed the
                # target before the target was known here)
                target = self.target
                if target is not None:
                    self.hit = self.first_past(self.polls, target)
                    if self.hit is not None:
                        return
                if time.monotonic() - t0 > self.timeout:
                    raise RuntimeError(f"the standby didn't replay in {self.timeout:.0f} s")
                time.sleep(self.interval)
        except BaseException as e:      # (raised again by wait())
            self.error = e

    def wait(self, target: int) -> tuple[Optional[float], float]:
        self.target = target
        self.thread.join(self.timeout + 10)
        if self.error is not None:
            raise self.error
        if self.hit is None:
            raise RuntimeError("the standby's replay watch didn't finish")
        return self.hit


def replay_times(hit: tuple[Optional[float], float], end_ts: float) -> dict[str, Optional[float]]:
    """A retire's replay time: at most how long after the retire ended
    (end_ts, the primary's clock) the standby had replayed it (<= 0: by
    then already), and the resolution, the time since the poll before."""
    prev, ts = hit
    return {"standby_replay_ms": 1000 * (ts - end_ts),
            "standby_replay_resolution_ms": None if prev is None else 1000 * (ts - prev)}


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

    # (set by prepare(); these defaults are the usual build's)
    standby = None
    wal_page, wal_segment = 8192, 16 << 20
    sections: dict[str, int] = {}

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
        self.wal_page = int(ctx.cluster.scalar("SELECT current_setting('wal_block_size')"))
        self.wal_segment = int(ctx.cluster.scalar(
            "SELECT setting::bigint FROM pg_settings WHERE name = 'wal_segment_size'"))
        self.prep = {"load_s": load_s, "base_commit_s": commit_s, "base_gc_s": gc_s,
                     "rows": n, **self.store(ctx)}
        self.standby = self.make_standby(ctx) if p.get("standby", False) else None
        (ctx.raw_dir / "v11-prepare.json").write_text(json.dumps(self.prep, indent=1) + "\n")

    def make_standby(self, ctx: Context):
        """A streaming standby from a base backup of the primary, and a
        session on it kept open for the replay polls."""
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
        self.standby = StandbySession(sb)
        self.wait_replayed(self.lsn(ctx))
        return self.standby

    # -- reading the store --------------------------------------------------

    def lsn(self, ctx) -> int:
        return lsn_int(ctx.cluster.scalar("SELECT pg_current_wal_insert_lsn()"))

    def wait_replayed(self, lsn: int, timeout: float = 600.0) -> None:
        """Until the standby had replayed to the primary's insert LSN lsn."""
        target = replay_target(lsn, self.wal_page, self.wal_segment)
        t0 = time.monotonic()
        while self.standby.poll()[1] < target:
            if time.monotonic() - t0 > timeout:
                raise RuntimeError(f"the standby didn't replay to {lsn:X} in {timeout:.0f} s")
            time.sleep(0.001)

    def store(self, ctx) -> dict[str, Any]:
        """The chunk store's sizes as its manifest has them; its files' bytes
        as the filesystem has them; whether the table files on disk, by name
        and size, or the count of journal segments, differ from what the
        manifest names (a leaked or missing file); its sections' bytes, read
        from the files' footers; the data directory's bytes without pg_wal,
        and pg_wal's."""
        out = self.store_sizes(ctx)
        out["store_bytes"] = out["journal_bytes"] + out["table_file_bytes"]
        named = self.table_file_bytes(ctx)
        data = Path(ctx.cluster.data_dir)
        dbdir = data / "base" / ctx.cluster.scalar(
            "SELECT oid FROM pg_database WHERE datname = current_database()", dbname=ctx.dbname)
        disk = store_files(dbdir)
        self.sections = disk["sections"]
        out["store_files_bytes_on_disk"] = disk["files_bytes"]
        out["store_meta_bytes_on_disk"] = disk["meta_bytes"]
        # (a journal segment grows as the fold worker or a commit appends
        # to it, between the manifest's read and the walk: its bytes are
        # only reported, and a difference is flagged only if the manifest
        # read again after the walk differs too)
        out["store_disk_minus_manifest_bytes"] = disk["files_bytes"] - out["store_bytes"]
        mismatch = disk["tables"] != named or disk["segments"] != out["journal_segments"]
        if mismatch:
            again = self.store_sizes(ctx)
            mismatch = (disk["tables"] != self.table_file_bytes(ctx) or
                        disk["segments"] != again["journal_segments"])
        out["store_files_mismatch"] = mismatch
        out["section_bytes_on_disk"] = sum(disk["sections"].values())
        out["table_files_on_disk"] = len(disk["sections"])
        out["data_dir_bytes"] = dir_bytes(data, skip=("pg_wal",))
        out["pg_wal_bytes"] = dir_bytes(data / "pg_wal")
        out["relation_bytes"] = int(ctx.cluster.scalar(
            "SELECT pg_total_relation_size('vc_rows')", dbname=ctx.dbname))
        return out

    def store_sizes(self, ctx) -> dict[str, Any]:
        return json.loads(ctx.cluster.scalar(
            "SELECT json_build_object('journal_segments', journal_segments, 'journal_bytes', journal_bytes,"
            " 'table_files', table_files, 'table_file_bytes', table_file_bytes,"
            " 'settled_files', settled_files, 'settled_bytes', settled_bytes) FROM prolly_store_sizes()",
            dbname=ctx.dbname))

    def generations(self, ctx) -> list[dict[str, Any]]:
        if not self.retire:
            return []
        s = ctx.cluster.scalar(
            "SELECT coalesce(json_agg(g ORDER BY epoch_lo, name), '[]') FROM test_chunk_generations() g",
            dbname=ctx.dbname)
        return json.loads(s)

    def table_file_bytes(self, ctx) -> dict[str, int]:
        return json.loads(ctx.cluster.scalar(
            "SELECT coalesce(json_object_agg(name, bytes), '{}') FROM test_chunk_table_files()",
            dbname=ctx.dbname))

    def counters(self, ctx) -> dict[str, int]:
        out = ctx.cluster.sql(SESSIONS_EXITED + "\nSELECT pg_stat_force_next_flush();\n"
                              "SELECT name || '=' || value FROM pg_stat_prolly",
                              dbname=ctx.dbname).stdout.split()
        return {k: int(v) for k, v in (line.split("=", 1) for line in out if "=" in line)}

    def point_reads(self, ctx, commit: str, reads: int, seed: int) -> dict[str, Any]:
        """reads point reads of commit by key, in one new session: each
        one's time, and the chunk-store reads they took (those alone: the
        mount's and the unmount's are outside the counts, and a counter
        session's own are taken off)."""
        n = self.rows(ctx)
        # (distinct keys, spread, a different set each call)
        keys = [1 + (seed * 7919 + i * (n // reads + 1)) % n for i in range(reads)]
        # (a snapshot mount of the commit, which seeks its tree by the key;
        # mounted and unmounted around the reads, so its pin holds nothing
        # past them)
        ctx.cluster.sql(f"SELECT prolly_mount('{commit}', 'versioned', 'v11_at', ARRAY['vc_rows'])",
                        dbname=ctx.dbname)
        try:
            # (the counts after the first counter session's exit less those
            # before it are what one counter session adds itself)
            idle = self.counters(ctx)
            before = self.counters(ctx)
            got = timed(ctx.cluster, ctx.dbname,
                        [f"SELECT id FROM v11_at.vc_rows WHERE id = {k}" for k in keys])
            after = self.counters(ctx)
        finally:
            ctx.cluster.sql("SELECT prolly_unmount('v11_at')", dbname=ctx.dbname)
        for (rows, _), k in zip(got, keys):
            if rows != [str(k)]:
                raise RuntimeError(f"point read of {k} at {commit} returned {rows}")
        ms = [t for _, t in got]
        d = {k: (after.get(k, 0) - before.get(k, 0)) - (before.get(k, 0) - idle.get(k, 0))
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
        sections_before = dict(self.sections)
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
        # (all the table-file bytes the cycle wrote, the default
        # collection's and the retire's, against the sections of the files
        # the cycle made)
        rec["cycle_table_file_bytes_written"] = rec["counters"].get("table_file_bytes_written", 0)
        rec.update(self.store(ctx))
        rec["new_section_bytes"] = sum(v for k, v in self.sections.items() if k not in sections_before)
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
        # (the younger files' section entries, whatever they name: how many
        # of them name R isn't known here)
        out.update(retired=True, retire_lo=lo, retire_hi=hi, retire_files=len(r_files),
                   retire_r_chunks=sum(g["chunks"] for g in r_files),
                   retire_r_garbage_est=sum(g["garbage"] for g in r_files),
                   retire_younger_section_entries=sum(g["xrefs"] for g in gens_before if g["epoch_lo"] > hi))
        names = {g["name"] for g in r_files}
        sizes = self.table_file_bytes(ctx)
        out["retire_r_bytes"] = sum(v for k, v in sizes.items() if k in names)
        watch = None
        if self.standby is not None:
            self.wait_replayed(self.lsn(ctx))
            watch = ReplayWatch(self.standby.poll).start()
        try:
            # (the LSNs and the end's time in the retire's own session)
            (start, _), (rows, ms), (end, _) = timed(ctx.cluster, db, [
                "SELECT pg_current_wal_insert_lsn()",
                f"SELECT kept || ' ' || freed || ' ' || bytes_freed FROM test_chunk_retire({lo}, {hi})",
                "SELECT extract(epoch FROM clock_timestamp()) || ' ' || pg_current_wal_insert_lsn()"])
        except BaseException:
            if watch is not None:
                watch.target = 0
            raise
        lsn0 = start[0]
        end_ts, lsn1 = end[0].split()
        out["retire_ms"] = ms
        out["retire_kept"], out["retire_freed"], out["retire_bytes_freed"] = (int(x) for x in rows[0].split())
        out["retire_wal_bytes"] = lsn_int(lsn1) - lsn_int(lsn0)
        if watch is not None:
            out.update(replay_times(watch.wait(replay_target(lsn_int(lsn1), self.wal_page, self.wal_segment)),
                                    float(end_ts)))
        gens_after = self.generations(ctx)
        after = {g["name"] for g in gens_after}
        out["retire_files_dropped_or_rewritten"] = len(names - after)
        # (R's own share of the bytes freed: its files gone, less the files
        # its survivors were rewritten to, which keep R's generations; the
        # rest is the young set's, collected and copied with it)
        sizes_after = self.table_file_bytes(ctx)
        rewritten = [g["name"] for g in gens_after if g["name"] not in sizes and g["settled"]
                     and lo <= g["epoch_lo"] and g["epoch_hi"] <= hi]
        out["retire_r_bytes_freed"] = (sum(sizes[k] for k in names - after)
                                       - sum(sizes_after.get(k, 0) for k in rewritten))
        out["retire_young_bytes_freed"] = out["retire_bytes_freed"] - out["retire_r_bytes_freed"]
        # (a file kept whole is still there, its estimate now the garbage
        # the retire found in it)
        kept = [g for g in gens_after if g["name"] in names]
        out["retire_files_kept_whole"] = len(kept)
        out["retire_kept_whole_chunks"] = sum(g["chunks"] for g in kept)
        out["retire_kept_whole_garbage"] = sum(g["garbage"] for g in kept)
        return out

    def measure(self, ctx: Context, window: Window) -> Outcome:
        p = ctx.params
        churn = int(p.get("churn", 10000))
        warm, cycles = warm_cycles(p), int(p.get("cycles", 10))
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
        try:
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
        finally:
            log.close()
            if self.standby is not None:
                self.standby.close()
        (ctx.raw_dir / "v11-end.json").write_text(json.dumps(end, indent=1) + "\n")
        return Outcome(metrics=self.summary(recs, end), transactions=len(recs), unit="cycle",
                       notes=[f"V11: {len(recs)} cycles; per-cycle numbers in v11-cycles.jsonl"])

    def finish(self, ctx, recs) -> dict[str, Any]:
        """After the cycles, on the store as they left it: point reads, a
        conjoin of the table files, and point reads again; then a full
        collection, every file with garbage rewritten (so what it frees is
        all the garbage the retires left; a conjoin copies garbage with the
        rest)."""
        p = ctx.params
        db = ctx.dbname
        end: dict[str, Any] = {}
        files = int(p.get("conjoin_files", 0))
        if files and recs:
            commit = recs[-1]["read_commit"]
            end["before_conjoin"] = self.store(ctx)
            end["reads_before_conjoin"] = self.point_reads(ctx, commit, int(p.get("reads", 200)), 10007)
            ((_, ms),) = timed(ctx.cluster, db, [f"SELECT test_chunk_conjoin({files})"])
            end["conjoin_ms"] = ms
            end["after_conjoin"] = self.store(ctx)
            end["reads_after_conjoin"] = self.point_reads(ctx, commit, int(p.get("reads", 200)), 10007)
        if p.get("full", True):
            before = self.store(ctx)
            (_, (rows, ms)) = timed(ctx.cluster, db, [
                "SET prolly.gc_rewrite_below = 2147483647",
                "SELECT chunks_kept || ' ' || chunks_freed || ' ' || bytes_freed FROM prolly_gc(true)"])
            kept, freed, bytes_freed = (int(x) for x in rows[0].split())
            after = self.store(ctx)
            end.update(full_gc_ms=ms, full_gc_kept=kept, full_gc_freed=freed,
                       full_gc_bytes_freed=bytes_freed, before_full=before, after_full=after)
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
        for name in ("retire_ms", "retire_r_bytes", "retire_bytes_freed", "retire_r_bytes_freed",
                     "retire_young_bytes_freed", "retire_wal_bytes", "standby_replay_ms",
                     "standby_replay_resolution_ms", "retire_files"):
            m[f"steady_{name}_median"] = median([r.get(name) for r in ret])
        m["steady_standby_replay_resolution_ms_max"] = max(
            (r["standby_replay_resolution_ms"] for r in ret
             if r.get("standby_replay_resolution_ms") is not None), default=None)
        m["steady_retire_ms_max"] = max((r["retire_ms"] for r in ret), default=None)
        for name in ("default_gc_ms", "commit_ms", "update_ms", "sever_ms"):
            m[f"steady_{name}_median"] = median([r[name] for r in steady])
        for k in sorted({r["churn"] for r in recs if r["phase"].startswith("sweep")} |
                        ({steady[0]["churn"]} if steady else set())):
            rs = [r for r in recs if r["churn"] == k and r["phase"] != "warm" and r.get("retired")]
            m[f"churn{k}_retire_ms_median"] = median([r["retire_ms"] for r in rs])
            m[f"churn{k}_retire_r_bytes_median"] = median([r["retire_r_bytes"] for r in rs])
        # (a retire's cost against the bytes it collected: a sweep cycle
        # retires a generation made window_cycles cycles before, at the
        # churn then)
        done = [r for r in recs if r["phase"] != "warm" and r.get("retired")]
        m["retire_ms_per_mib_slope"] = slope([r["retire_r_bytes"] / 2**20 for r in done],
                                             [r["retire_ms"] for r in done])
        m["retires_kept_whole"] = sum(1 for r in done if r.get("retire_files_kept_whole"))
        m["retired_r_bytes_freed_total"] = sum(r.get("retire_r_bytes_freed", 0) for r in recs)
        section = [r.get("new_file_xrefs") for r in steady if r.get("new_file_xrefs") is not None]
        m["steady_section_entries_per_cycle_median"] = median(section)
        m["store_files_mismatch_cycles"] = sum(1 for r in recs if r.get("store_files_mismatch"))
        if steady:
            # (the sections of the files a cycle made, against all the
            # table-file bytes it wrote)
            written = sum(r["cycle_table_file_bytes_written"] for r in steady)
            m["steady_section_bytes_per_cycle_median"] = median([r["new_section_bytes"] for r in steady])
            m["section_bytes_per_gb_written"] = (sum(r["new_section_bytes"] for r in steady) / written * 2**30
                                                 if written else None)
            m["default_gc_bytes_written_steady"] = sum(r["default_gc_table_file_bytes_written"] for r in steady)
            m["cycle_table_file_bytes_written_steady"] = written
            m["store_bytes_first_steady"] = steady[0]["store_bytes"]
            m["store_bytes_last_steady"] = steady[-1]["store_bytes"]
            xs = [r["cycle"] for r in steady]
            for name in ("store_bytes", "store_files_bytes_on_disk", "data_dir_bytes", "pg_wal_bytes"):
                m[f"{name}_slope_per_cycle"] = slope(xs, [r[name] for r in steady])
            m["table_files_first_steady"] = steady[0]["table_files"]
            m["table_files_last_steady"] = steady[-1]["table_files"]
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
