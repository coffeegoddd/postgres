-- A first use's move planner costs each candidate's copies with the tables
-- a kept materialization holds shared, not written: copies cheapest to
-- move once those are shared are chosen over copies cheaper to move row by
-- row without them.

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
	FROM unnest(ARRAY['apply_rows', 'move_tables_shared']) c
$$;

-- (none kept from earlier tests)
SET prolly.kept_checkouts = 0;
SELECT (dolt_checkout('main'))[2];
RESET prolly.kept_checkouts;

CREATE SCHEMA kp VERSIONED;
CREATE TABLE kp.a (id int PRIMARY KEY, v int);
CREATE TABLE kp.b (id int PRIMARY KEY, v int);
INSERT INTO kp.a SELECT g, g FROM generate_series(1, 4000) g;
INSERT INTO kp.b SELECT g, g FROM generate_series(1, 4000) g;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
-- x: a negated, 300 of b's rows changed; t: a as x has it, b as base has it;
-- main: a as base has it, b as t has it.
UPDATE kp.a SET v = -v;
UPDATE kp.b SET v = v + 10000 WHERE id <= 300;
SELECT (dolt_commit('-am', 'x'))[1] AS x \gset
UPDATE kp.b SET v = id;
SELECT (dolt_commit('-am', 't'))[1] AS t \gset
UPDATE kp.a SET v = id;
SELECT length(dolt_commit('-am', 'main')::text) AS committed;

-- x kept.  t from main: main's copies differ from t in all of a, which x
-- has as t does; x's copies differ from t in 300 of b's rows, which no kept
-- state has.  Main's, a then shared, cost less: no row is written.
SELECT dolt_checkout(:'x') IS NOT NULL AS detached;
SELECT (dolt_checkout('main'))[2];
:flush
SELECT count(*) >= 0 AS counting FROM pg_temp.work();
SELECT dolt_checkout(:'t') IS NOT NULL AS detached;
:flush
SELECT * FROM pg_temp.work();
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_t;
SELECT count(*) FILTER (WHERE v < 0) AS a_negated FROM kp.a;
SELECT count(*) FILTER (WHERE v <> id) AS b_changed FROM kp.b;

SELECT (dolt_checkout('main'))[2];
SET prolly.kept_checkouts = 0;
SELECT (dolt_checkout('main'))[2];
RESET prolly.kept_checkouts;
DROP SCHEMA kp CASCADE;
