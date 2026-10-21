-- Moves to a state another branch holds (#491).  A branch's first use
-- copies the branch nearest its head: one whose working set is known to be
-- it (nothing then written or folded), else one whose baseline's fold or
-- head made it (only what that branch changed since is written), else the
-- session's.  A move to a root another branch's working set is known to be
-- gives each table whose rows differ that branch's pages (its indexes',
-- TOAST table's, change log's and maps with them), where sharing its
-- blocks costs less than writing the rows; the rest are written.
-- written(): the rows the transaction wrote in ns, by table (the session's
-- counts flushed before it began: :flush, a statement of its own);
-- since(counter): pg_stat_prolly's counter's growth since it was last
-- asked (flushed first too).
CREATE DATABASE regress_nearest_source;
\c regress_nearest_source
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
CREATE EXTENSION amcheck;
RESET client_min_messages;
SET dolt.author = 'Ada Lovelace <ada@example.com>';
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE FUNCTION pg_temp.written() RETURNS TABLE (relname name, n int8)
  LANGUAGE sql AS $$
	SELECT relname, n_tup_ins + n_tup_upd + n_tup_del FROM pg_stat_xact_user_tables
	WHERE schemaname = 'ns' ORDER BY relname
  $$;
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION pg_temp.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;
CREATE SCHEMA ns VERSIONED;
CREATE TABLE ns.t (id int PRIMARY KEY, v text);
CREATE TABLE ns.u (id int PRIMARY KEY, w text);
INSERT INTO ns.t SELECT g, 'v' || g FROM generate_series(1, 20000) g;
INSERT INTO ns.u SELECT g, 'w' || g FROM generate_series(1, 100) g;
SELECT length(dolt_commit('-Am', 'base')::text) > 0 AS committed;

-- feat: 5000 of t's rows changed, committed, clean.
SELECT dolt_branch('feat');
SELECT dolt_checkout('feat');
UPDATE ns.t SET v = 'feat' WHERE id <= 5000;
SELECT length(dolt_commit('-am', 'feat')::text) > 0 AS committed;
SELECT count(*) AS changes FROM dolt_status;
SELECT dolt_checkout('main');

-- feat with a change of its own, left (its checkpoint folded): its head
-- is the nearest, and only its change is written back.
SELECT dolt_checkout('feat');
UPDATE ns.u SET w = 'dirty' WHERE id = 1;
SELECT dolt_checkout('main');
SELECT dolt_branch('at_feat', 'feat');
:flush
BEGIN;
SELECT dolt_checkout('at_feat');
SELECT * FROM pg_temp.written();
COMMIT;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_head;
SELECT w FROM ns.u WHERE id = 1;
SELECT dolt_checkout('feat');
SELECT w FROM ns.u WHERE id = 1;
SELECT dolt_reset('--hard');
SELECT count(*) AS changes FROM dolt_status;
SELECT dolt_checkout('main');

-- A first use of a branch at feat's head, from main, feat clean again:
-- feat's copies, nothing written, and their working set feat's, known
-- (taken, not folded).
SELECT dolt_branch('at_feat2', 'feat');
:flush
SELECT pg_temp.since('folds_reused') >= 0 AS counting;
BEGIN;
SELECT dolt_checkout('at_feat2');
SELECT * FROM pg_temp.written();
COMMIT;
:flush
SELECT pg_temp.since('folds_reused') AS reused;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_head;
SELECT count(*) FILTER (WHERE v = 'feat') AS feat_rows, count(*) FROM ns.t;
SELECT count(*) AS changes FROM dolt_status;
SELECT dolt_checkout('main');

-- main fast-forwarded to feat, whose working set is known to be its head:
-- t's 5000 rows given feat's pages, nothing written; then the working set
-- is the target root, and t's index agrees with its pages.
UPDATE ns.u SET w = 'main' WHERE id = 2;	-- (u's one row: written)
SELECT length(dolt_commit('-am', 'main')::text) > 0 AS committed;
SELECT dolt_reset('--hard', 'HEAD~1');
:flush
BEGIN;
SELECT (dolt_merge('feat'))[4] AS merged;
SELECT * FROM pg_temp.written();
COMMIT;
SELECT test_fold_root() = prolly_root_hash('feat') AS at_target;
SELECT bt_index_check('ns.t_pkey', true);
SELECT count(*) FILTER (WHERE v = 'feat') AS feat_rows, count(*) FROM ns.t;
SELECT count(*) AS changes FROM dolt_status;

-- The pages are shared until either branch writes them: main's write
-- isn't feat's, and folds go on from feat's baseline.
UPDATE ns.t SET v = 'main' WHERE id = 7;
SELECT table_name, status FROM dolt_status;
SELECT length(dolt_commit('-am', 'after')::text) > 0 AS committed;
SELECT dolt_checkout('feat');
SELECT v FROM ns.t WHERE id = 7;
SELECT count(*) AS changes FROM dolt_status;
SELECT dolt_checkout('main');
\c
SET dolt.author = 'Ada Lovelace <ada@example.com>';
SELECT count(*) AS changes FROM dolt_status;
SELECT bt_index_check('ns.t_pkey', true);

-- A hard reset back to feat's head, once feat is known clean again:
-- shared again.  A move of a row (a reset of one written) writes it:
-- sharing u's blocks costs more.
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE FUNCTION pg_temp.written() RETURNS TABLE (relname name, n int8)
  LANGUAGE sql AS $$
	SELECT relname, n_tup_ins + n_tup_upd + n_tup_del FROM pg_stat_xact_user_tables
	WHERE schemaname = 'ns' ORDER BY relname
  $$;
SELECT dolt_reset('--hard', 'HEAD~2');
-- (feat folded again, as its dirtiness is read: what this session's
-- catalog changes since made unknown is known again)
SELECT name, dirty FROM dolt_branches WHERE name = 'feat';
:flush
BEGIN;
SELECT dolt_reset('--hard', 'feat');
SELECT * FROM pg_temp.written();
COMMIT;
SELECT test_fold_root() = prolly_root_hash('feat') AS at_target;
SELECT bt_index_check('ns.t_pkey', true);
UPDATE ns.u SET w = 'again' WHERE id = 3;
:flush
BEGIN;
SELECT dolt_reset('--hard');
SELECT * FROM pg_temp.written();
COMMIT;
SELECT test_fold_root() = prolly_root_hash('feat') AS at_target;

-- Two tables whose rows differ from a clean branch's, a hard reset to it:
-- each is given that branch's pages, not only the first (whose catalog
-- change keeps anything new from being known in the transaction).
CREATE TABLE ns.x (id int PRIMARY KEY, y text);
INSERT INTO ns.x SELECT g, 'y' || g FROM generate_series(1, 20000) g;
SELECT length(dolt_commit('-Am', 'two')::text) > 0 AS committed;
SELECT dolt_branch('two');
SELECT dolt_checkout('two');
UPDATE ns.t SET v = 'two' WHERE id <= 5000;
UPDATE ns.x SET y = 'two' WHERE id <= 5000;
SELECT length(dolt_commit('-am', 'two')::text) > 0 AS committed;
SELECT count(*) AS changes FROM dolt_status;
SELECT dolt_checkout('main');
SELECT name, dirty FROM dolt_branches WHERE name = 'two';
:flush
BEGIN;
SELECT dolt_reset('--hard', 'two');
SELECT * FROM pg_temp.written();
COMMIT;
SELECT test_fold_root() = prolly_root_hash('two') AS at_target;
SELECT bt_index_check('ns.t_pkey', true);
SELECT bt_index_check('ns.x_pkey', true);
SELECT count(*) FILTER (WHERE v = 'two') AS t_rows FROM ns.t;
SELECT count(*) FILTER (WHERE y = 'two') AS x_rows FROM ns.x;

\c contrib_regression
DROP DATABASE regress_nearest_source;
