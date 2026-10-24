-- Kept materializations: a detached checkout's hidden branch stays once
-- its sessions leave (prolly.kept_checkouts of them), so checking out its
-- commit again is a branch switch, and a move or first use elsewhere gives
-- a table whose rows differ the storage of a kept branch that has the
-- target's, instead of writing its rows.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
-- (the move planner's sources and the kept materializations' shares, not
-- pages kept as of the head, which come before them and move nothing:
-- first_use_kept tests those)
SET prolly.kept_commits = 0;

-- since(counter): pg_stat_prolly's counter's growth since it was last
-- asked, the session's counts flushed first (:flush, a statement of its
-- own).
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION pg_temp.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;
-- (catalog rows copied: whether any were)
CREATE FUNCTION pg_temp.work() RETURNS TABLE (counter text, grew text)
LANGUAGE sql AS $$
	SELECT c, CASE WHEN c = 'catalog_rows_copied' THEN (n > 0)::text
		ELSE n::text END
	FROM unnest(ARRAY['apply_rows', 'catalog_rows_copied', 'move_tables_shared',
					  'kept_checkouts_reused', 'kept_checkouts_evicted',
					  'bmap_release_queued']) c,
		LATERAL pg_temp.since(c) n
$$;
CREATE FUNCTION pg_temp.hidden() RETURNS int8 LANGUAGE sql AS
  $$SELECT count(*) FROM pg_branch WHERE brname LIKE 'detached/%'$$;

-- (none kept from earlier tests)
SET prolly.kept_checkouts = 0;
SELECT (dolt_checkout('main'))[2];
RESET prolly.kept_checkouts;
SELECT pg_temp.hidden() AS kept;

CREATE SCHEMA kc VERSIONED;
CREATE TABLE kc.big (id int PRIMARY KEY, v int, w text);
CREATE INDEX big_v ON kc.big (v);
CREATE TABLE kc.small (id int PRIMARY KEY, v text);
INSERT INTO kc.big SELECT g, g, 'w' || g FROM generate_series(1, 4000) g;
INSERT INTO kc.small SELECT g, 'v' || g FROM generate_series(1, 10) g;
SELECT (dolt_commit('-Am', 'c0'))[1] AS c0 \gset
-- c1: big's first 1000 rows and small changed; c2: small alone; c3 (main):
-- big's last 1000.  c1 and c2 hold the same big, which main doesn't.
UPDATE kc.big SET v = v + 1 WHERE id <= 1000;
UPDATE kc.small SET v = 'c1' WHERE id = 1;
SELECT (dolt_commit('-am', 'c1'))[1] AS c1 \gset
UPDATE kc.small SET v = 'c2' WHERE id = 1;
SELECT (dolt_commit('-am', 'c2'))[1] AS c2 \gset
UPDATE kc.big SET v = v + 1 WHERE id > 3000;
SELECT length(dolt_commit('-am', 'c3')::text) AS committed;
SHOW prolly.kept_checkouts;
:flush
SELECT count(*) >= 0 AS counting FROM pg_temp.work();

-- A first checkout of c1 makes its copies and writes c1's rows (the
-- planner reloads big, its 4000 rows and small's one).
SELECT (dolt_checkout(:'c1'))[2] = 'HEAD is now at ' || :'c1' || ' (detached, read-only)' AS detached;
:flush
SELECT * FROM pg_temp.work() WHERE counter <> 'bmap_release_queued';
SELECT count(*) FILTER (WHERE v <> id) AS changed, max(v) FROM kc.big;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_c1;

-- Leaving keeps it, and checking c1 out again copies and writes nothing.
SELECT (dolt_checkout('main'))[2];
SELECT pg_temp.hidden() AS kept;
:flush
SELECT count(*) >= 0 AS counting FROM pg_temp.work();
SELECT dolt_checkout(:'c1') IS NOT NULL AS detached;
:flush
SELECT * FROM pg_temp.work() WHERE counter <> 'bmap_release_queued';
SELECT count(*) FILTER (WHERE v <> id) AS changed, max(v) FROM kc.big;
SELECT v FROM kc.small WHERE id = 1;
-- (still read-only)
UPDATE kc.small SET v = 'no' WHERE id = 1;

-- c2, from main: big's rows differ from main's, and c1's kept branch has
-- c2's big, so the move planner takes c1's copies, its big shared whole,
-- and only small's one row is written.  (kept_planned.sql shares a kept
-- table into another branch's copies.)
SELECT (dolt_checkout('main'))[2];
:flush
SELECT count(*) >= 0 AS counting FROM pg_temp.work();
SELECT dolt_checkout(:'c2') IS NOT NULL AS detached;
:flush
SELECT * FROM pg_temp.work() WHERE counter <> 'bmap_release_queued';
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_c2;
SELECT count(*) FILTER (WHERE v <> id) AS changed, max(v) FROM kc.big;
SELECT v FROM kc.small WHERE id = 1;
-- (its indexes are c2's: the shared secondary index finds c2's rows)
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM kc.big WHERE v = 3;
SELECT id FROM kc.big WHERE v = 3;
SELECT id FROM kc.big WHERE v = 4001;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- A branch made at c2 and written copies the pages it writes: c1's and
-- c2's kept branches keep theirs.
SELECT (dolt_checkout('-b', 'from_c2'))[2];
UPDATE kc.big SET v = -1 WHERE id <= 1000;
SELECT length(dolt_commit('-am', 'from c2')::text) AS committed;
SELECT dolt_checkout(:'c1') IS NOT NULL AS detached;
SELECT count(*) FILTER (WHERE v = -1) AS written, count(*) FILTER (WHERE v <> id) AS changed FROM kc.big;
SELECT dolt_checkout(:'c2') IS NOT NULL AS detached;
SELECT count(*) FILTER (WHERE v = -1) AS written, count(*) FILTER (WHERE v <> id) AS changed FROM kc.big;

-- A hard reset of a branch to c1 shares big from a kept branch too.
SELECT (dolt_checkout('from_c2'))[2];
:flush
SELECT count(*) >= 0 AS counting FROM pg_temp.work();
SELECT dolt_reset('--hard', :'c1');
:flush
SELECT counter, grew FROM pg_temp.work()
WHERE counter IN ('apply_rows', 'move_tables_shared');
SELECT count(*) FILTER (WHERE v = -1) AS written, count(*) FILTER (WHERE v <> id) AS changed FROM kc.big;
SELECT v FROM kc.small WHERE id = 1;

-- Past the budget, the least recently checked out go as a checkout
-- sweeps: c1 was checked out after c2 here, so c2 goes first.
SELECT dolt_checkout(:'c1') IS NOT NULL AS detached;
SELECT (dolt_checkout('main'))[2];
SELECT pg_temp.hidden() AS kept;
:flush
SELECT count(*) >= 0 AS counting FROM pg_temp.work();
SET prolly.kept_checkouts = 1;
SELECT (dolt_checkout('main'))[2];
:flush
SELECT counter, grew FROM pg_temp.work() WHERE counter = 'kept_checkouts_evicted';
SELECT brname = 'detached/' || substr(:'c1', 1, 40) AS c1_kept
FROM pg_branch WHERE brname LIKE 'detached/%';
-- (a detached checkout sweeps too, the one it is on not counted: c0 is
-- kept beside c1, and c2 made again then lets c1, used before c0, go)
SELECT dolt_checkout(:'c0') IS NOT NULL AS detached;
SELECT count(*) FILTER (WHERE v <> id) AS changed FROM kc.big;
SELECT pg_temp.hidden() AS kept;
SELECT dolt_checkout(:'c2') IS NOT NULL AS detached;
SELECT substr(brname, 10) IN (substr(:'c0', 1, 40), substr(:'c2', 1, 40)) AS c0_or_c2
FROM pg_branch WHERE brname LIKE 'detached/%' ORDER BY 1;
-- (and none at all kept with 0, as before, their storage released:
-- queued, for autovacuum to free)
SET prolly.kept_checkouts = 0;
:flush
SELECT count(*) >= 0 AS counting FROM pg_temp.work();
SELECT (dolt_checkout('main'))[2];
SELECT pg_temp.hidden() AS kept;
:flush
SELECT counter, CASE WHEN counter = 'bmap_release_queued'
	THEN (grew::int8 > 0)::text ELSE grew END AS grew
FROM pg_temp.work() WHERE counter IN ('kept_checkouts_evicted', 'bmap_release_queued');
RESET prolly.kept_checkouts;

-- A rolled back checkout of a kept commit doesn't leave the session
-- there.  DROP ROLE lets go of idle kept checkouts only when something
-- depends on the role: c1's, kept from before the grant, holds no copy of
-- it, so it stays (t/216_kept_checkout_budget.pl drops a role a kept
-- checkout's copies name).
CREATE ROLE kc_reader;
GRANT SELECT ON kc.small TO kc_reader;
SELECT dolt_checkout(:'c1') IS NOT NULL AS detached;
SELECT (dolt_checkout('main'))[2];
BEGIN;
SELECT dolt_checkout(:'c1') IS NOT NULL AS detached;
ROLLBACK;
SELECT active_branch();
SELECT pg_temp.hidden() AS kept;
REVOKE SELECT ON kc.small FROM kc_reader;
DROP ROLE kc_reader;
SELECT pg_temp.hidden() AS kept;

SELECT dolt_branch('-D', 'from_c2');
DROP SCHEMA kc CASCADE;
