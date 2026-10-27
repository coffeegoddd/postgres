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
