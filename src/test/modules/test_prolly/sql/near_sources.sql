-- A branch's first use when no branch is at its head: the copies made are
-- those of the branch the move planner estimates is cheapest to move
-- there (from its known working set, its baseline's fold or its head), so
-- a branch a few rows from the commit is copied rather than a session's
-- branch far from it.  A branch whose definitions differ is costed as
-- writing its tables again; the session's branch is kept on a tie; and at
-- most 16 branches beside it are weighed.
-- since(counter): pg_stat_prolly's counter's growth since it was last
-- asked (the session's counts flushed first, :flush).
CREATE DATABASE regress_near_sources;
\c regress_near_sources
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
CREATE EXTENSION amcheck;
RESET client_min_messages;
SET dolt.author = 'Ada Lovelace <ada@example.com>';
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION pg_temp.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;
CREATE SCHEMA ns VERSIONED;
CREATE TABLE ns.t (id int PRIMARY KEY, v text);
CREATE INDEX t_v ON ns.t (v);
CREATE TABLE ns.u (id int PRIMARY KEY, w text);
INSERT INTO ns.t SELECT g, 'v' || g FROM generate_series(1, 20000) g;
INSERT INTO ns.u SELECT g, 'w' || g FROM generate_series(1, 100) g;
SELECT length(dolt_commit('-Am', 'base')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS target \gset

-- near: 200 of t's rows from the target, committed, clean.
SELECT dolt_branch('near');
SELECT dolt_checkout('near');
UPDATE ns.t SET v = 'near' WHERE id <= 200;
SELECT length(dolt_commit('-am', 'near')::text) > 0 AS committed;

-- reshaped: as few rows from it, but t has a column more.
SELECT dolt_checkout('main');
SELECT dolt_branch('reshaped');
SELECT dolt_checkout('reshaped');
ALTER TABLE ns.t ADD COLUMN x int;
UPDATE ns.t SET v = 'reshaped' WHERE id <= 10;
SELECT length(dolt_commit('-am', 'reshaped')::text) > 0 AS committed;

-- main: 10000 rows from it.
SELECT dolt_checkout('main');
UPDATE ns.t SET v = 'far' WHERE id % 2 = 0;
SELECT length(dolt_commit('-am', 'far')::text) > 0 AS committed;

-- The target checked out from main: near's copies, its 200 rows written
-- back, not main's 10000.
:flush
SELECT pg_temp.since('apply_rows') >= 0 AS counting,
	pg_temp.since('move_plans') >= 0 AS counting;
SELECT (dolt_checkout(:'target'))[1] AS status;
:flush
SELECT pg_temp.since('apply_rows') AS rows_written,
	pg_temp.since('move_plans') AS plans;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_target;
SELECT count(*) FILTER (WHERE v = 'v' || id) AS as_committed, count(*) FROM ns.t;
SELECT count(*) AS columns FROM information_schema.columns
WHERE table_schema = 'ns' AND table_name = 't';
SELECT bt_index_check('ns.t_pkey', true), bt_index_check('ns.t_v', true);
SELECT dolt_checkout('main');

-- near dirty, its working set known (folded as its dirtiness is read) to
-- be further from the target than main: main's copies, its 10000 rows
-- written back.
SELECT dolt_checkout('near');
UPDATE ns.t SET v = 'dirty' WHERE id > 200;
SELECT dolt_checkout('main');
SELECT name, dirty FROM dolt_branches WHERE name = 'near';
SELECT dolt_branch('at_target', :'target');
:flush
SELECT pg_temp.since('apply_rows') >= 0 AS counting;
SELECT dolt_checkout('at_target');
:flush
SELECT pg_temp.since('apply_rows') AS rows_written;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_target;
SELECT count(*) FILTER (WHERE v = 'v' || id) AS as_committed, count(*) FROM ns.t;
SELECT dolt_checkout('main');
SELECT dolt_checkout('near');
SELECT dolt_reset('--hard');
SELECT dolt_checkout('main');

-- Many branches: main and 16 others weighed, however many there are.
-- (last's head is a commit main left, which no branch is at.)
SELECT count(dolt_branch('b' || g)) FROM generate_series(1, 30) g;
SELECT count(*) FROM generate_series(1, 30) g,
	LATERAL (SELECT dolt_checkout('b' || g)) c;
SELECT dolt_checkout('main');
UPDATE ns.u SET w = 'left' WHERE id = 1;
SELECT length(dolt_commit('-am', 'left')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS left \gset
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_branch('last', :'left');
:flush
SELECT pg_temp.since('move_plans') >= 0 AS counting;
SELECT dolt_checkout('last');
:flush
SELECT pg_temp.since('move_plans') AS plans;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_target;

\c contrib_regression
DROP DATABASE regress_near_sources;

-- A materialized view's rows are costed as a table's: a branch whose
-- tables are the target's but whose view was refreshed over other rows
-- (each of its 20000 to be written back) is further than main, 2000 of
-- whose table's rows differ.
CREATE DATABASE regress_near_views;
\c regress_near_views
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
SET dolt.author = 'Ada Lovelace <ada@example.com>';
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION pg_temp.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;
CREATE SCHEMA ns VERSIONED;
CREATE TABLE ns.t (id int PRIMARY KEY, v text);
INSERT INTO ns.t SELECT g, 'v' || g FROM generate_series(1, 20000) g;
CREATE MATERIALIZED VIEW ns.mv AS SELECT id, v FROM ns.t;
SELECT length(dolt_commit('-Am', 'base')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS target \gset
SELECT dolt_branch('refreshed');
SELECT dolt_checkout('refreshed');
UPDATE ns.t SET v = 'x' || id;
REFRESH MATERIALIZED VIEW ns.mv;
UPDATE ns.t SET v = 'v' || id;
SELECT length(dolt_commit('-am', 'refreshed')::text) > 0 AS committed;
SELECT dolt_checkout('main');
UPDATE ns.t SET v = 'far' WHERE id <= 2000;
SELECT length(dolt_commit('-am', 'far')::text) > 0 AS committed;
:flush
SELECT pg_temp.since('apply_rows') >= 0 AS counting;
SELECT (dolt_checkout(:'target'))[1] AS status;
:flush
SELECT pg_temp.since('apply_rows') AS rows_written;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_target;
SELECT count(*) FILTER (WHERE v = 'v' || id) AS as_committed FROM ns.mv;

\c contrib_regression
DROP DATABASE regress_near_views;
