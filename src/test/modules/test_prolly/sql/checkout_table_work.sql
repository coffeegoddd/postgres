-- A checkout's work, table by table: each table whose rows differ from
-- the target's is either given another state's storage
-- (move_tables_shared) or has its rows written (move_tables_written); a
-- table that doesn't differ costs neither.  What a lazy preparation of
-- tables would defer is the second.

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
CREATE FUNCTION pg_temp.work() RETURNS TABLE (counter text, grew int8)
LANGUAGE sql AS $$
	SELECT c, pg_temp.since(c)
	FROM unnest(ARRAY['move_tables_written', 'move_tables_shared',
					  'apply_rows']) c
$$;

CREATE SCHEMA cw VERSIONED;
CREATE TABLE cw.a (id int PRIMARY KEY, v int);
CREATE TABLE cw.b (id int PRIMARY KEY, v int);
CREATE TABLE cw.same (id int PRIMARY KEY, v int);
INSERT INTO cw.a SELECT g, g FROM generate_series(1, 3000) g;
INSERT INTO cw.b SELECT g, g FROM generate_series(1, 3000) g;
INSERT INTO cw.same SELECT g, g FROM generate_series(1, 10) g;
SELECT (dolt_commit('-Am', 'base'))[1] AS base \gset
-- main: a and b changed after base
UPDATE cw.a SET v = -v WHERE id <= 1000;
UPDATE cw.b SET v = -v WHERE id <= 1000;
SELECT length(dolt_commit('-am', 'changed')::text) AS committed;

-- base, first visited: both differ, both written.
:flush
SELECT count(*) >= 0 AS counting FROM pg_temp.work();
SELECT dolt_checkout(:'base') IS NOT NULL AS detached;
:flush
SELECT * FROM pg_temp.work();

-- A commit with base's a and a b of its own, base kept: a is shared, b
-- written, same neither.
SELECT (dolt_checkout('main'))[2];
SELECT dolt_branch('other', :'base');
SELECT dolt_checkout('other');
UPDATE cw.b SET v = 0 WHERE id <= 10;
SELECT (dolt_commit('-am', 'other'))[1] AS other \gset
SELECT (dolt_checkout('main'))[2];
-- (and no branch is at it, to share b from)
SELECT dolt_tag('other_tag', :'other');
SELECT dolt_branch('-D', 'other');
:flush
SELECT count(*) >= 0 AS counting FROM pg_temp.work();
SELECT dolt_checkout(:'other') IS NOT NULL AS detached;
:flush
SELECT * FROM pg_temp.work();
SELECT count(*) FILTER (WHERE v < 0) AS a_changed FROM cw.a;
SELECT count(*) FILTER (WHERE v = 0) AS b_zeroed, count(*) FILTER (WHERE v < 0) AS b_changed FROM cw.b;

SELECT (dolt_checkout('main'))[2];
SELECT dolt_tag('-d', 'other_tag');
SET prolly.kept_checkouts = 0;
SELECT (dolt_checkout('main'))[2];
RESET prolly.kept_checkouts;
DROP SCHEMA cw CASCADE;
