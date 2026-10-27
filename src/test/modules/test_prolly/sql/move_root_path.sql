-- The path a move takes, as pg_stat_prolly counts it: the pages kept as of
-- its commit given back (moves_root_kept), those of the nearest kept
-- commit before it and the rows between written (moves_root_nearest), or
-- its rows written for want of either (moves_root_missing).  And reads
-- refused, or rows left out, for history past the horizon
-- (severed_refusals).  prolly_history_roots() lists the commits whose
-- pages are kept, so a caller can tell beforehand which path a move takes.

SELECT current_database() AS home \gset
\set VERBOSITY terse

-- A root every third commit, in a window that holds them all.
CREATE DATABASE regress_move_root_path;
ALTER DATABASE regress_move_root_path SET prolly.history_window = '1000 years';
ALTER DATABASE regress_move_root_path SET prolly.root_interval_commits = 3;
\c regress_move_root_path
-- path(): the counters that tell a move's path, as flushed
CREATE FUNCTION path() RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$
	SELECT name, value FROM pg_stat_prolly
	WHERE name IN ('moves_root_kept', 'moves_root_nearest',
				   'moves_root_missing', 'move_tables_written',
				   'severed_refusals')
	ORDER BY name
$$;
CREATE FUNCTION mr_err(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN regexp_replace(SQLERRM, '[0-9a-f]{40}', '<hash>', 'g');
END $$;
CREATE SCHEMA mr VERSIONED;
CREATE TABLE mr.t (id int PRIMARY KEY, v int NOT NULL);
INSERT INTO mr.t SELECT g, 0 FROM generate_series(1, 1000) g;
SELECT dolt_hashof('HEAD') IS NOT NULL AS made
FROM (SELECT dolt_commit('-Am', 'c0')) c;
SELECT dolt_hashof('HEAD') AS c0 \gset
UPDATE mr.t SET v = 1 WHERE id <= 10;
SELECT length(dolt_commit('-Am', 'c1')::text) > 0 AS c1_made;
SELECT dolt_hashof('HEAD') AS c1 \gset
UPDATE mr.t SET v = 2 WHERE id <= 10;
SELECT length(dolt_commit('-Am', 'c2')::text) > 0 AS c2_made;
SELECT dolt_hashof('HEAD') AS c2 \gset
UPDATE mr.t SET v = 3 WHERE id <= 10;
SELECT length(dolt_commit('-Am', 'c3')::text) > 0 AS c3_made;
SELECT dolt_hashof('HEAD') AS c3 \gset
UPDATE mr.t SET v = 4 WHERE id <= 10;
SELECT length(dolt_commit('-Am', 'c4')::text) > 0 AS c4_made;

-- The roots kept: c3 and c0, newest first; c3's kept by main, c2's by none.
SELECT kept_by, CASE kept_commit WHEN :'c0' THEN 'c0' WHEN :'c3' THEN 'c3'
	ELSE kept_commit END AS kept
FROM prolly_history_roots('main', NULL);
SELECT kept_by, kept_commit = :'c3' AS is_c3 FROM prolly_history_roots(NULL, :'c3');
SELECT count(*) AS c2_roots FROM prolly_history_roots(NULL, :'c2');
SELECT count(*) AS other_branch_c3 FROM prolly_history_roots('other', :'c3');
SELECT mr_err('SELECT * FROM prolly_history_roots(NULL, NULL)') AS neither;
SELECT mr_err($$SELECT * FROM prolly_history_roots(NULL, 'zz')$$) AS not_a_hash;

-- A move to c3, kept: its pages given back, no rows written.
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT dolt_reset('--hard', :'c3');
SELECT pg_stat_force_next_flush();
SELECT * FROM path();
SELECT count(*) FILTER (WHERE v = 3) AS at_c3 FROM mr.t;

-- A move to c2, not kept: c0's pages given back, and the rows between.
SELECT pg_stat_reset_shared('prolly');
SELECT dolt_reset('--hard', :'c2');
SELECT pg_stat_force_next_flush();
SELECT * FROM path();
SELECT count(*) FILTER (WHERE v = 2) AS at_c2 FROM mr.t;

-- A branch's first use at c0, kept: its tables made of c0's pages, so
-- the move's restore has none left to give back and writes no rows; it
-- counts as kept, not as a move without a root.
SELECT dolt_branch('at_c0', :'c0');
SELECT pg_stat_reset_shared('prolly');
SELECT dolt_checkout('at_c0');
SELECT pg_stat_force_next_flush();
SELECT * FROM path();
SELECT count(*) FILTER (WHERE v = 0) AS at_c0 FROM mr.t;
SELECT dolt_checkout('main');

-- Another branch's root of a schema the session's branch no longer has
-- isn't usable: a branch made at c2 that adds a column, moved to c3, which
-- only main keeps, counts no kept root, but the nearest before it or none.
SELECT dolt_branch('reshaped', :'c2');
SELECT dolt_checkout('reshaped');
ALTER TABLE mr.t ADD COLUMN w int;
SELECT length(dolt_commit('-Am', 'reshaped')::text) > 0 AS reshaped_made;
SELECT count(*) AS reshaped_keeps_c3 FROM prolly_history_roots('reshaped', :'c3');
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT dolt_reset('--hard', :'c3');
SELECT pg_stat_force_next_flush();
SELECT name, value FROM path() WHERE name LIKE 'moves_root_%';
SELECT count(*) FILTER (WHERE v = 3) AS at_c3,
	count(*) FILTER (WHERE id = 1) AS one_row
FROM mr.t;
SELECT count(*) AS no_w FROM information_schema.columns
WHERE table_schema = 'mr' AND table_name = 't' AND column_name = 'w';
SELECT dolt_checkout('main');

-- Roots capped at a byte: the oldest let go of, so a move to c1 has
-- neither its own nor one before it, and writes its rows.
\c :home
CREATE DATABASE regress_move_root_missing;
ALTER DATABASE regress_move_root_missing SET prolly.history_window = '1000 years';
ALTER DATABASE regress_move_root_missing SET prolly.history_root_max_size = '1';
\c regress_move_root_missing
CREATE FUNCTION path() RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$
	SELECT name, value FROM pg_stat_prolly
	WHERE name IN ('moves_root_kept', 'moves_root_nearest',
				   'moves_root_missing', 'move_tables_written',
				   'severed_refusals')
	ORDER BY name
$$;
CREATE SCHEMA mr VERSIONED;
CREATE TABLE mr.t (id int PRIMARY KEY, v int NOT NULL);
INSERT INTO mr.t SELECT g, 0 FROM generate_series(1, 1000) g;
SELECT length(dolt_commit('-Am', 'c0')::text) > 0 AS c0_made;
UPDATE mr.t SET v = 1 WHERE id <= 10;
SELECT length(dolt_commit('-Am', 'c1')::text) > 0 AS c1_made;
SELECT dolt_hashof('HEAD') AS c1 \gset
UPDATE mr.t SET v = 2 WHERE id <= 10;
SELECT length(dolt_commit('-Am', 'c2')::text) > 0 AS c2_made;
UPDATE mr.t SET v = 3 WHERE id <= 10;
SELECT length(dolt_commit('-Am', 'c3')::text) > 0 AS c3_made;
SELECT count(*) AS c1_roots FROM prolly_history_roots(NULL, :'c1');
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT dolt_reset('--hard', :'c1');
SELECT pg_stat_force_next_flush();
SELECT * FROM path();
SELECT count(*) FILTER (WHERE v = 1) AS at_c1 FROM mr.t;

-- History past the horizon: a history table's rows of it left out, and a
-- read of its data refused, each counted.
\c :home
CREATE DATABASE regress_move_root_severed;
ALTER DATABASE regress_move_root_severed SET prolly.history_window = '1000 years';
\c regress_move_root_severed
CREATE FUNCTION path() RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$
	SELECT name, value FROM pg_stat_prolly
	WHERE name IN ('moves_root_kept', 'moves_root_nearest',
				   'moves_root_missing', 'move_tables_written',
				   'severed_refusals')
	ORDER BY name
$$;
CREATE FUNCTION mr_err(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN regexp_replace(SQLERRM, '[0-9a-f]{40}', '<hash>', 'g');
END $$;
CREATE FUNCTION mr_commit(msg text, day int) RETURNS text LANGUAGE sql AS $$
	SELECT trim(both '{}' from dolt_commit('-Am', msg, '--date',
		(timestamptz '2020-01-01 00:00+00' + make_interval(days => day))::text)::text)
$$;
CREATE SCHEMA mr VERSIONED;
CREATE TABLE mr.t (id int PRIMARY KEY, v text);
INSERT INTO mr.t VALUES (1, 'c1');
SELECT mr_commit('c1', 1) AS c1 \gset
UPDATE mr.t SET v = 'c2';
SELECT mr_commit('c2', 2) AS c2 \gset
UPDATE mr.t SET v = 'c3';
SELECT mr_commit('c3', 3) AS c3 \gset
UPDATE mr.t SET v = 'c4';
SELECT mr_commit('c4', 4) AS c4 \gset
SELECT chunks_freed >= 0 AS collected FROM prolly_gc(true);
SELECT commits FROM dolt_history_drop('2020-01-03 00:00+00');
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
-- (the read is answered, its severed commits' rows left out)
SELECT count(*) AS history_rows FROM dolt_history_table(NULL::mr.t);
SELECT pg_stat_force_next_flush();
SELECT value > 0 AS rows_left_out_counted FROM path()
WHERE name = 'severed_refusals';
SELECT chunks_freed > 0 AS freed FROM prolly_gc(true);
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT mr_err(format('SELECT count(*) FROM dolt_at(%L, NULL::mr.t)', :'c1')) AS at_c1;
SELECT pg_stat_force_next_flush();
SELECT value > 0 AS refusal_counted FROM path()
WHERE name = 'severed_refusals';
-- (a read of history inside it counts none)
SELECT pg_stat_reset_shared('prolly');
SELECT count(*) AS at_c4 FROM dolt_at(:'c4', NULL::mr.t);
SELECT pg_stat_force_next_flush();
SELECT value AS in_window_refusals FROM path()
WHERE name = 'severed_refusals';

\c :home
DROP DATABASE regress_move_root_path;
DROP DATABASE regress_move_root_missing;
DROP DATABASE regress_move_root_severed;
