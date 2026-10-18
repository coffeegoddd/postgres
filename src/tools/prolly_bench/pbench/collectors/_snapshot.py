"""Reading cumulative statistics for counters.

A backend flushes its statistics lazily (about once a second) and when it
exits, so a reading taken while the measured clients are connected can lag
their work.  With settled_counters, a reading first waits, in the session
that reads, until no other client backend is connected: every backend that
did the work has exited, and so flushed.  (Its own session's statistics,
the reading's, aren't flushed yet, and the next reading's session waits
for them; each reading's are the same, so they cancel between two runs.)
"""

from __future__ import annotations

# (pg_stat_clear_snapshot: a transaction otherwise keeps the activity it
# first read)
_WAIT = """DO $pbench$
BEGIN
  FOR i IN 1..{tries} LOOP
    PERFORM pg_stat_clear_snapshot();
    EXIT WHEN NOT EXISTS (SELECT FROM pg_stat_activity
                          WHERE backend_type = 'client backend'
                            AND pid <> pg_backend_pid());
    PERFORM pg_sleep(0.01);
  END LOOP;
  PERFORM pg_stat_clear_snapshot();
  IF EXISTS (SELECT FROM pg_stat_activity
             WHERE backend_type = 'client backend' AND pid <> pg_backend_pid()) THEN
    RAISE EXCEPTION 'measured clients still connected after % s', {tries} / 100;
  END IF;
END
$pbench$;
"""


def settled(ctx) -> bool:
    return bool(ctx.params.get("settled_counters"))


def snapshot(ctx, sql: str, *, dbname: str = "postgres", timeout_seconds: int = 120) -> str:
    """sql's single result, read as the counters' runs need it."""
    if not settled(ctx):
        return ctx.cluster.scalar(sql, dbname=dbname)
    script = _WAIT.format(tries=timeout_seconds * 100) + sql.rstrip().rstrip(";") + ";\n"
    return ctx.cluster.sql(script, dbname=dbname).stdout.strip()
