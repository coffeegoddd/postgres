-- A commit the head leaves other than forward (a reset or an amend away
-- from it) keeps its root until the move away is older than the history
-- window, however long the head was at it: so a move back to it within the
-- window is O(1).  And the window's min_commits counts the newest kept on
-- the head's first parents, not a commit reset away from.
SELECT current_database() AS home \gset
CREATE DATABASE kept_departures;
ALTER DATABASE kept_departures SET prolly.history_window = '3 seconds';
\c kept_departures
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
\set flush 'SELECT pg_stat_force_next_flush() IS NULL AS flushed \\gset'
CREATE FUNCTION counted() RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(name, value) FROM pg_stat_prolly
	WHERE name IN ('apply_rows', 'relations_restored')
$$;
CREATE FUNCTION since(before jsonb) RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$
	SELECT key, value::bigint - (before ->> key)::bigint
	FROM jsonb_each_text(counted()) ORDER BY 1
$$;
CREATE FUNCTION kept(c text) RETURNS bool LANGUAGE sql AS $$
	SELECT EXISTS (SELECT FROM test_kept_commits('main') WHERE commit_id = c)
$$;

CREATE SCHEMA kd VERSIONED;
CREATE TABLE kd.t (id int PRIMARY KEY, v int NOT NULL);
INSERT INTO kd.t SELECT g, 0 FROM generate_series(1, 2000) g;
SELECT dolt_commit('-Am', 'c0') IS NOT NULL AS committed;
UPDATE kd.t SET v = 1;
SELECT dolt_commit('-am', 'c1') IS NOT NULL AS committed;
UPDATE kd.t SET v = 2;
SELECT dolt_commit('-am', 'c2') IS NOT NULL AS committed;
SELECT dolt_hashof('main') AS c2 \gset

-- the head at c2 longer than the window, then reset away from it
SELECT pg_sleep(4);
SELECT dolt_reset('--hard', 'HEAD~1') IS NOT NULL AS reset;
SELECT kept(:'c2') AS left_keeps_root;
-- (a pass now, as the next keep's would be: c2 left just now, in the window)
SELECT test_retain_roots('main');
SELECT kept(:'c2') AS still_kept;
-- and the move back is O(1): no rows written
:flush
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'c2') IS NOT NULL AS back;
:flush
SELECT * FROM since(:'before');
SELECT sum(v) = 4000 AS c2_rows FROM kd.t;

-- once the move away is older than the window, its root is let go of
SELECT dolt_reset('--hard', 'HEAD~1') IS NOT NULL AS reset;
SELECT pg_sleep(4);
SELECT test_retain_roots('main');
SELECT kept(:'c2') AS let_go;

-- min_commits: the newest kept on the head's first parents.  Reset from
-- r5 back to r2 and committed on to n2 (r4's height): r3 is below the
-- head's height, r4 beside it and r5 above, none on its history, and none
-- takes a place of n2, n1 or r2.
ALTER DATABASE kept_departures SET prolly.history_window = '1 microsecond';
ALTER DATABASE kept_departures SET prolly.history_min_commits = 3;
\c kept_departures
CREATE FUNCTION mk(tag text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	UPDATE kd.t SET v = v + 1 WHERE id = 1;
	RETURN dolt_commit('-am', tag);
END $$;
SELECT mk('r1') IS NOT NULL AS r1;
SELECT mk('r2') IS NOT NULL AS r2;
SELECT mk('r3') AS r3 \gset
SELECT mk('r4') AS r4 \gset
SELECT mk('r5') AS r5 \gset
SELECT dolt_reset('--hard', 'HEAD~3') IS NOT NULL AS reset;
SELECT mk('n1') IS NOT NULL AS n1;
SELECT mk('n2') IS NOT NULL AS n2;
SELECT test_retain_roots('main');
SELECT kept(dolt_hashof('main')) AS n2_kept,
	kept(dolt_hashof('main~1')) AS n1_kept,
	kept(dolt_hashof('main~2')) AS r2_kept;
SELECT kept(:'r3') AS r3_kept, kept(:'r4') AS r4_kept, kept(:'r5') AS r5_kept;

\c :home
DROP DATABASE kept_departures;

-- Another branch's root serves this one's only if a move could be given
-- it: one captured among writers, not yet frozen, can't be (only its own
-- branch freezes it), so b keeps its own frozen root of x while main's
-- head is at x with an unfrozen one.
CREATE DATABASE kept_departures_other;
ALTER DATABASE kept_departures_other SET prolly.history_window = '1000 years';
\c kept_departures_other
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
CREATE FUNCTION kept(b text, c text) RETURNS bool LANGUAGE sql AS $$
	SELECT EXISTS (SELECT FROM test_kept_commits(b) WHERE commit_id = c)
$$;
CREATE SCHEMA ko VERSIONED;
CREATE TABLE ko.t (id int PRIMARY KEY, v int NOT NULL);
INSERT INTO ko.t SELECT g, 0 FROM generate_series(1, 500) g;
SELECT dolt_commit('-Am', 'base') IS NOT NULL AS committed;
SELECT dolt_branch('b');
-- x: main's, kept unfrozen (each transaction a commit)
SET dolt.transaction_commit = on;
SET prolly.root_interval = 0;
UPDATE ko.t SET v = 1;
RESET dolt.transaction_commit;
RESET prolly.root_interval;
SELECT dolt_hashof('main') AS x \gset
-- b moved to x keeps a frozen root of it, then commits on
SELECT dolt_checkout('b');
SELECT dolt_reset('--hard', :'x') IS NOT NULL AS reset;
SELECT kept('b', :'x') AS b_keeps_x;
UPDATE ko.t SET v = 2;
SELECT dolt_commit('-am', 'y') IS NOT NULL AS committed;
ALTER DATABASE kept_departures_other SET prolly.history_window = '1 microsecond';
SELECT test_retain_roots('b');
SELECT kept('b', :'x') AS b_still_keeps_x;
\c :home
DROP DATABASE kept_departures_other;

-- A third branch's keep of a commit, its head elsewhere: a move on
-- another branch is given its pages (the refs' kept_holders()), a count
-- of the commit's log reads its recorded count, and a sever forgets its
-- keep with every other branch's.
CREATE DATABASE kept_departures_third;
\c kept_departures_third
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
CREATE FUNCTION counted() RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(name, value) FROM pg_stat_prolly
	WHERE name IN ('apply_rows', 'move_tables_shared', 'commits_read')
$$;
CREATE FUNCTION since(before jsonb) RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$
	SELECT key, value::bigint - coalesce((before ->> key)::bigint, 0)
	FROM jsonb_each_text(counted()) ORDER BY 1
$$;
CREATE FUNCTION kept(b text, c text) RETURNS bool LANGUAGE sql AS $$
	SELECT EXISTS (SELECT FROM test_kept_commits(b) WHERE commit_id = c)
$$;
CREATE SCHEMA k3 VERSIONED;
CREATE TABLE k3.t (id int PRIMARY KEY, v int NOT NULL);
INSERT INTO k3.t SELECT g, 0 FROM generate_series(1, 2000) g;
-- commits dated long ago, one a day: all past a sever before 2020-06
CREATE FUNCTION old(tag text, i int) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	UPDATE k3.t SET v = i WHERE id <= 20;
	RETURN dolt_commit('-am', tag || i, '--date',
		(timestamptz '2020-01-01 00:00+00' + make_interval(days => i))::text);
END $$;
SELECT dolt_commit('-Am', 'base', '--date', '2020-01-01 00:00+00') IS NOT NULL AS committed;
SELECT length(old('old', i)) > 0 AS committed FROM generate_series(1, 20) i;
SELECT dolt_hashof('main') AS y \gset
SELECT length(old('old', i)) > 0 AS committed FROM generate_series(21, 30) i;
SELECT dolt_hashof('main') AS x \gset
SELECT dolt_branch('b');
SELECT dolt_branch('c');
SELECT length(old('main', i)) > 0 AS committed FROM generate_series(31, 32) i;
UPDATE k3.t SET v = -1 WHERE id = 2000;
SELECT dolt_commit('-am', 'main on') IS NOT NULL AS committed;
-- b and c keep y by a move back to it, then go on from x: y is below
-- every branch's fork, the commits a sever before 2020-06 cuts
SELECT dolt_checkout('b');
SELECT dolt_reset('--hard', :'y') IS NOT NULL AS reset;
SELECT dolt_reset('--hard', :'x') IS NOT NULL AS reset;
SELECT length(old('b', i)) > 0 AS committed FROM generate_series(41, 42) i;
UPDATE k3.t SET v = -2 WHERE id = 1999;
SELECT dolt_commit('-am', 'b on') IS NOT NULL AS committed;
SELECT dolt_checkout('c');
SELECT dolt_reset('--hard', :'y') IS NOT NULL AS reset;
SELECT dolt_reset('--hard', :'x') IS NOT NULL AS reset;
SELECT length(old('c', i)) > 0 AS committed FROM generate_series(51, 52) i;
UPDATE k3.t SET v = -3 WHERE id = 1998;
SELECT dolt_commit('-am', 'c on') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
SELECT kept('main', :'y') AS main_keeps, kept('b', :'y') AS b_keeps,
	kept('c', :'y') AS c_keeps;
-- (main's own let go of, so only other branches keep x)
ALTER DATABASE kept_departures_third SET prolly.history_window = '1 microsecond';
SELECT test_retain_roots('main');
ALTER DATABASE kept_departures_third RESET prolly.history_window;
SELECT kept('main', :'y') AS main_keeps, kept('b', :'y') AS b_keeps;
-- the count: another branch's recorded one, no walk of the history
SELECT pg_stat_force_next_flush() IS NULL AS flushed \gset
SELECT counted() AS before \gset
SELECT count(*) AS y_commits FROM dolt_log(:'y');
SELECT pg_stat_force_next_flush() IS NULL AS flushed \gset
SELECT value <= 2 AS count_read_few FROM since(:'before') WHERE name = 'commits_read';
-- a move on main to x: another branch's pages shared, no rows written
SELECT dolt_hashof('main') AS main_on \gset
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'y') IS NOT NULL AS reset;
SELECT pg_stat_force_next_flush() IS NULL AS flushed \gset
SELECT name, value > 0 AS counted FROM since(:'before')
WHERE name IN ('apply_rows', 'move_tables_shared') ORDER BY name;
-- a sever past x: every branch's keep of x forgotten
SELECT dolt_reset('--hard', :'main_on') IS NOT NULL AS back;
SELECT count(*) > 0 AS severed FROM dolt_history_drop('2020-06-01 00:00+00');
SELECT kept('main', :'y') AS main_keeps, kept('b', :'y') AS b_keeps,
	kept('c', :'y') AS c_keeps;
\c :home
DROP DATABASE kept_departures_third;
