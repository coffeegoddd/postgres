-- A move shares another branch's storage for a table only if both store it
-- alike.  Equal definitions don't make equal layouts: a column added and
-- dropped on one branch leaves its number taken there, so the same table
-- defined the same way stores its columns at other numbers, and the other
-- branch's tuples read by this one's descriptor would put a column's
-- value in the wrong place.

-- A database of its own, so that a reset moves this test's table alone (a
-- move restores every table with a root, those earlier tests left too).
SELECT current_database() AS home \gset
CREATE DATABASE regress_share_layouts;
\c regress_share_layouts
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

\set flush 'SELECT pg_stat_force_next_flush();'
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION pg_temp.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;

CREATE SCHEMA sl VERSIONED;
CREATE TABLE sl.t (id int PRIMARY KEY, v text);
INSERT INTO sl.t SELECT g, 'v' || g FROM generate_series(1, 3000) g;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('lay');

-- main: w added.
ALTER TABLE sl.t ADD COLUMN w text DEFAULT 'w';
SELECT length(dolt_commit('-am', 'w on main')::text) AS committed;

-- lay: x added (with a value in every row) and dropped, then w: the same
-- definition as main's, w stored as column 4.
SELECT dolt_checkout('lay');
ALTER TABLE sl.t ADD COLUMN x int;
UPDATE sl.t SET x = id;
ALTER TABLE sl.t DROP COLUMN x;
ALTER TABLE sl.t ADD COLUMN w text DEFAULT 'w';
UPDATE sl.t SET v = 'lay' WHERE id <= 2000;
SELECT (dolt_commit('-am', 'lay'))[1] AS l1 \gset
SELECT count(*) AS changes FROM dolt_status;

-- main moved to lay's commit: the same definitions, 2000 rows differ, and
-- lay is at the commit, but stores t otherwise, so the 2000 rows are
-- written.  (Shared, its pages would read w as another column, and every
-- row would be written again once that was found.)
SELECT dolt_checkout('main');
SELECT count(*) AS changes FROM dolt_status;
:flush
SELECT pg_temp.since(c) IS NOT NULL AS counting
FROM unnest(ARRAY['move_tables_shared', 'apply_rows']) c;
SELECT dolt_reset('--hard', :'l1');
:flush
SELECT c AS counter, pg_temp.since(c) AS grew
FROM unnest(ARRAY['move_tables_shared', 'apply_rows']) c;
SELECT count(*) FILTER (WHERE w = 'w') AS w_read, count(*) FILTER (WHERE w IS NULL) AS w_null,
	count(*) FILTER (WHERE v = 'lay') AS from_lay FROM sl.t;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_target;

-- Stored alike (a branch made from main now), it is shared.
SELECT dolt_branch('alike');
SELECT dolt_checkout('alike');
UPDATE sl.t SET v = 'alike' WHERE id <= 2000;
SELECT (dolt_commit('-am', 'alike'))[1] AS a1 \gset
SELECT count(*) AS changes FROM dolt_status;
SELECT dolt_checkout('main');
:flush
SELECT pg_temp.since('move_tables_shared') IS NOT NULL AS counting;
SELECT dolt_reset('--hard', :'a1');
:flush
SELECT pg_temp.since('move_tables_shared') AS shared;
SELECT count(*) FILTER (WHERE w = 'w') AS w_read, count(*) FILTER (WHERE v = 'alike') AS from_alike
FROM sl.t;

SELECT dolt_branch('-D', 'lay', 'alike');
DROP SCHEMA sl CASCADE;

\c :home
DROP DATABASE regress_share_layouts;
