-- dolt.transaction_commit: what its commits can't hold, what they fall
-- back to the working set for, and what they cost.  A database of its own,
-- so its engine counters count only this file's work.
SELECT current_database() AS home \gset
CREATE DATABASE regress_tcc;
\c regress_tcc
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
CREATE SCHEMA tcc VERSIONED;
CREATE TABLE tcc.t (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
INSERT INTO tcc.t SELECT g, 'r' || g FROM generate_series(1, 5) g;
SELECT length(dolt_commit('-Am', 'tcc base')::text) AS committed;
CREATE FUNCTION pg_temp.commits() RETURNS bigint LANGUAGE sql
  AS $$ SELECT count(*) FROM dolt_log('main') $$;
CREATE FUNCTION pg_temp.head_rows() RETURNS text LANGUAGE sql
  AS $$ SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM dolt_at('HEAD', NULL::tcc.t) $$;
SET dolt.transaction_commit = on;

-- Objects a commit can't hold fail COMMIT, and the transaction with it.
SELECT pg_temp.commits() AS before \gset
CREATE OPERATOR tcc.=== (LEFTARG = int, RIGHTARG = int, FUNCTION = int4eq);
BEGIN;
INSERT INTO tcc.t VALUES (6, 'six');
CREATE TEXT SEARCH DICTIONARY tcc.dict (TEMPLATE = pg_catalog.simple);
COMMIT;
SELECT pg_temp.commits() - :before AS commits,
	(SELECT count(*) FROM pg_operator WHERE oprnamespace = 'tcc'::regnamespace) AS operators,
	(SELECT count(*) FROM tcc.t WHERE id = 6) AS rows;
-- (made outside the mode, one stays in the working set while the mode
-- commits rows; a change of definitions commits the working set, and fails)
SET dolt.transaction_commit = off;
CREATE OPERATOR tcc.=== (LEFTARG = int, RIGHTARG = int, FUNCTION = int4eq);
SET dolt.transaction_commit = on;
INSERT INTO tcc.t VALUES (6, 'six');
SELECT pg_temp.commits() - :before AS commits;
SELECT * FROM dolt_status;
CREATE TABLE tcc.u (id int);
SELECT pg_temp.commits() - :before AS commits;
SET dolt.transaction_commit = off;
DROP OPERATOR tcc.=== (int, int);
SET dolt.transaction_commit = on;

-- Rewrites commit the working set: TRUNCATE, CLUSTER and VACUUM FULL move
-- or drop rows without recording them, as a rewriting ALTER TABLE does.
-- Each commit is the branch as the transaction leaves it (the branch is
-- clean after it), rows recorded before the rewrite included.
SELECT pg_temp.commits() AS before \gset
BEGIN;
INSERT INTO tcc.t VALUES (7, 'seven');
TRUNCATE tcc.t;
INSERT INTO tcc.t VALUES (1, 'after truncate'), (2, 'two'), (3, 'three');
COMMIT;
SELECT pg_temp.commits() - :before AS commits, pg_temp.head_rows() AS head,
	(SELECT dirty FROM dolt_branches() WHERE name = 'main') AS dirty;
BEGIN;
UPDATE tcc.t SET v = 'updated' WHERE id = 2;
INSERT INTO tcc.t VALUES (9, 'nine');
CLUSTER tcc.t USING t_pkey;
DELETE FROM tcc.t WHERE id = 3;
COMMIT;
SELECT pg_temp.commits() - :before AS commits, pg_temp.head_rows() AS head,
	(SELECT dirty FROM dolt_branches() WHERE name = 'main') AS dirty;
-- (alone, a rewrite changes no rows, and makes no commit)
CLUSTER tcc.t;
VACUUM FULL tcc.t;
SELECT pg_temp.commits() - :before AS commits,
	(SELECT dirty FROM dolt_branches() WHERE name = 'main') AS dirty;
BEGIN;
INSERT INTO tcc.t VALUES (10, 'ten');
ALTER TABLE tcc.t ADD COLUMN n int DEFAULT (random() * 0)::int;
UPDATE tcc.t SET n = id;
COMMIT;
ALTER TABLE tcc.t ALTER COLUMN n TYPE bigint;
SELECT pg_temp.commits() - :before AS commits,
	(SELECT dirty FROM dolt_branches() WHERE name = 'main') AS dirty;
SELECT id, v, n FROM dolt_at('HEAD', NULL::tcc.t) ORDER BY id;
ALTER TABLE tcc.t DROP COLUMN n;
-- (with rows written outside the mode, the fallback's commit holds them
-- too, as the working set does)
SET dolt.transaction_commit = off;
INSERT INTO tcc.t VALUES (20, 'outside');
SET dolt.transaction_commit = on;
BEGIN;
INSERT INTO tcc.t VALUES (21, 'mine');
CLUSTER tcc.t;
COMMIT;
SELECT pg_temp.head_rows() AS head,
	(SELECT dirty FROM dolt_branches() WHERE name = 'main') AS dirty;

-- A merge in progress commits the working set, a merge commit with both
-- parents, whatever else the transaction wrote.
SELECT dolt_branch('tcc_side');
SELECT dolt_checkout('tcc_side');
UPDATE tcc.t SET v = 'side' WHERE id = 1;
SELECT dolt_checkout('main');
UPDATE tcc.t SET v = 'main' WHERE id = 1;
SELECT dolt_hashof('tcc_side') AS side, dolt_hashof('HEAD') AS ours \gset
BEGIN;
SELECT (dolt_merge('tcc_side'))[3] AS conflicts;
SELECT dolt_conflicts_resolve('--theirs', 'tcc.t');
INSERT INTO tcc.t VALUES (30, 'in the merge');
COMMIT;
SELECT dolt_hashof('HEAD^1') = :'ours' AS first_parent_ours,
	dolt_hashof('HEAD^2') = :'side' AS second_parent_theirs,
	(SELECT message FROM dolt_log('main') LIMIT 1) AS message,
	(SELECT dirty FROM dolt_branches() WHERE name = 'main') AS dirty;
SELECT id, v FROM dolt_at('HEAD', NULL::tcc.t) WHERE id IN (1, 30) ORDER BY id;

-- Constraint violations alone a merge leaves: kept (dolt.allow_commit_
-- conflicts on), the merge commit holds them; otherwise they fail COMMIT,
-- and no commit is made.
CREATE TABLE tcc.parent (id int PRIMARY KEY);
CREATE TABLE tcc.child (id int PRIMARY KEY, p int REFERENCES tcc.parent);
INSERT INTO tcc.parent VALUES (1), (2);
-- (a branch forced to the head is the move's change, as a reset's is, not
-- the transaction's: it neither commits nor counts as a branch left)
SELECT dolt_branch('-f', 'tcc_side');
SELECT dolt_hashof('tcc_side') = dolt_hashof('HEAD') AS forced;
-- (but a branch the transaction wrote before the move stays its write:
-- COMMIT still refuses a branch left, and the rows go with the transaction)
SELECT dolt_hashof('tcc_side') AS side \gset
BEGIN;
SELECT dolt_checkout('tcc_side');
INSERT INTO tcc.t VALUES (40, 'left behind');
SELECT dolt_checkout('main');
SELECT dolt_branch('-f', 'tcc_side');
COMMIT;
SELECT dolt_hashof('tcc_side') = :'side' AS side_unmoved,
	(SELECT dirty FROM dolt_branches() WHERE name = 'tcc_side') AS side_dirty;
SELECT dolt_checkout('tcc_side');
DELETE FROM tcc.parent WHERE id = 2;
SELECT dolt_checkout('main');
INSERT INTO tcc.child VALUES (1, 2);
SELECT pg_temp.commits() AS before \gset
SELECT dolt_hashof('HEAD') AS ours \gset
SET dolt.allow_commit_conflicts = on;
BEGIN;
SELECT (dolt_merge('tcc_side'))[3] AS conflicts;
COMMIT;
RESET dolt.allow_commit_conflicts;
SELECT pg_temp.commits() - :before AS commits,
	dolt_hashof('HEAD^2') = dolt_hashof('tcc_side') AS merged;
SELECT * FROM dolt_constraint_violations();
SELECT dolt_reset('--hard', :'ours');
BEGIN;
SELECT (dolt_merge('tcc_side'))[3] AS conflicts;
COMMIT;
SELECT dolt_hashof('HEAD') = :'ours' AS no_commit;
DROP TABLE tcc.child, tcc.parent;

-- Whether a transaction commits so is settled at its first write.
SELECT pg_temp.commits() AS before \gset
BEGIN;
INSERT INTO tcc.t VALUES (50, 'in the mode');
SET dolt.transaction_commit = off;
INSERT INTO tcc.t VALUES (51, 'still');
COMMIT;
SELECT pg_temp.commits() - :before AS commits,
	(SELECT count(*) FROM dolt_at('HEAD', NULL::tcc.t) WHERE id IN (50, 51)) AS committed;
BEGIN;
INSERT INTO tcc.t VALUES (52, 'not in the mode');
SET dolt.transaction_commit = on;
INSERT INTO tcc.t VALUES (53, 'still not');
COMMIT;
SELECT pg_temp.commits() - :before AS commits,
	(SELECT dirty FROM dolt_branches() WHERE name = 'main') AS dirty;
SET dolt.transaction_commit = off;
SELECT length(dolt_commit('-am', 'tcc rows')::text) AS committed;
SET dolt.transaction_commit = on;

-- A version replaced in a subtransaction rolled back, and replaced again,
-- is recorded twice: committed once (a keyless copy deleted so is one
-- copy fewer, not two).
CREATE TABLE tcc.copies (v int);
INSERT INTO tcc.copies VALUES (1), (1), (1);
SELECT dolt_hashof('HEAD') AS before \gset
BEGIN;
SAVEPOINT s;
UPDATE tcc.t SET v = 'undone' WHERE id = 2;
DELETE FROM tcc.copies WHERE ctid = (SELECT min(ctid) FROM tcc.copies);
ROLLBACK TO s;
UPDATE tcc.t SET v = 'twice' WHERE id = 2;
DELETE FROM tcc.copies WHERE ctid = (SELECT min(ctid) FROM tcc.copies);
COMMIT;
SELECT diff_type, (from_row).v AS from_v, (to_row).v AS to_v
  FROM dolt_diff(:'before', 'HEAD', NULL::tcc.t);
SELECT count(*) AS copies FROM dolt_at('HEAD', NULL::tcc.copies);
SELECT name, dirty FROM dolt_branches() WHERE name = 'main';

-- Left and checked out again, the branch a transaction changed is the
-- session's once more, and the transaction commits on it (Dolt's commit to
-- another branch, once the session is on it).
SELECT pg_temp.commits() AS before \gset
BEGIN;
SELECT dolt_checkout('tcc_side');
INSERT INTO tcc.t VALUES (40, 'on side');
SELECT dolt_checkout('main');
SELECT dolt_checkout('tcc_side');
COMMIT;
SELECT active_branch(), pg_temp.commits() - :before AS main_commits,
	(SELECT message FROM dolt_log('tcc_side') LIMIT 1) AS side_message;
SELECT id, v FROM dolt_at('tcc_side', NULL::tcc.t) WHERE id = 40;
SELECT dolt_checkout('main');

-- With one writer, a commit of the transaction's own rows is the commit a
-- fold of the working set makes: the same workload on two branches from
-- one commit, one committing its rows (as by default) and one folding
-- (no row changes kept), gives the same root at every commit.
CREATE TABLE tcc.h (id serial PRIMARY KEY, v text, big text STORAGE EXTERNAL)
  WITH (autovacuum_enabled = off);
CREATE TABLE tcc.bag (v int) WITH (autovacuum_enabled = off);
SET dolt.transaction_commit = off;
INSERT INTO tcc.h (v) SELECT 'r' || g FROM generate_series(1, 20) g;
INSERT INTO tcc.bag SELECT g % 3 FROM generate_series(1, 9) g;
SELECT length(dolt_commit('-Am', 'tcc workload')::text) AS committed;
SET dolt.transaction_commit = on;
SELECT dolt_branch('tcc_rows');
SELECT dolt_branch('tcc_fold');
SELECT pg_temp.commits() AS before \gset
\set workload 'INSERT INTO tcc.h (v) VALUES (''new''); UPDATE tcc.h SET v = ''u'' WHERE id % 4 = 0; DELETE FROM tcc.h WHERE id = 3; BEGIN; INSERT INTO tcc.bag VALUES (1), (1); DELETE FROM tcc.bag WHERE ctid = (SELECT min(ctid) FROM tcc.bag WHERE v = 2); SAVEPOINT s; DELETE FROM tcc.h; ROLLBACK TO s; UPDATE tcc.h SET big = repeat(''x'', 50000) WHERE id = 5; COMMIT; UPDATE tcc.bag SET v = 7 WHERE v = 0; BEGIN; UPDATE tcc.h SET big = repeat(''y'', 50000) WHERE id = 5; INSERT INTO tcc.h (v) VALUES (''two''); DELETE FROM tcc.bag WHERE v = 7; COMMIT;'
SELECT dolt_checkout('tcc_rows');
:workload
SELECT dolt_checkout('tcc_fold');
SET dolt.transaction_commit_max_changes = 0;
:workload
RESET dolt.transaction_commit_max_changes;
SELECT dolt_checkout('main');
SELECT (SELECT count(*) FROM dolt_log('tcc_rows')) - :before AS rows_commits,
	(SELECT count(*) FROM dolt_log('tcc_fold')) - :before AS fold_commits;
SELECT k, prolly_root_hash('tcc_rows~' || k) = prolly_root_hash('tcc_fold~' || k) AS same_root
  FROM generate_series(0, 6) k;

-- A commit's cost is its own rows': a row updated with 20,000 rows changed
-- outside the mode on the branch costs what it costs on a clean one.
CREATE TABLE tcc.o (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
CREATE TABLE tcc.w (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
SET dolt.transaction_commit = off;
INSERT INTO tcc.w SELECT g, 'v' || g FROM generate_series(1, 2000) g;
SELECT length(dolt_commit('-Am', 'tcc cost')::text) AS committed;
SET dolt.transaction_commit = on;
-- (Each is measured three times, and the least of each counter taken: the
-- counters are the database's, and what another process does for it
-- meanwhile, under load, only adds.)
CREATE TEMP TABLE cost (k text, name text, value bigint);
SELECT pg_stat_force_next_flush() IS NULL AS flushed;
SELECT pg_stat_reset_shared('prolly') IS NULL AS reset;
UPDATE tcc.w SET v = 'clean' WHERE id = 10;
SELECT pg_stat_force_next_flush() IS NULL AS flushed;
INSERT INTO cost SELECT 'clean', name, value FROM pg_stat_prolly;
SELECT pg_stat_force_next_flush() IS NULL AS flushed;
SELECT pg_stat_reset_shared('prolly') IS NULL AS reset;
UPDATE tcc.w SET v = 'clean' WHERE id = 13;
SELECT pg_stat_force_next_flush() IS NULL AS flushed;
INSERT INTO cost SELECT 'clean', name, value FROM pg_stat_prolly;
SELECT pg_stat_force_next_flush() IS NULL AS flushed;
SELECT pg_stat_reset_shared('prolly') IS NULL AS reset;
UPDATE tcc.w SET v = 'clean' WHERE id = 14;
SELECT pg_stat_force_next_flush() IS NULL AS flushed;
INSERT INTO cost SELECT 'clean', name, value FROM pg_stat_prolly;
SET dolt.transaction_commit = off;
INSERT INTO tcc.o SELECT g, 'outside' || g FROM generate_series(1, 20000) g;
UPDATE tcc.w SET v = 'outside' WHERE id > 1000;
SET dolt.transaction_commit = on;
SELECT pg_stat_force_next_flush() IS NULL AS flushed;
SELECT pg_stat_reset_shared('prolly') IS NULL AS reset;
UPDATE tcc.w SET v = 'busy' WHERE id = 11;
SELECT pg_stat_force_next_flush() IS NULL AS flushed;
INSERT INTO cost SELECT 'busy', name, value FROM pg_stat_prolly;
SELECT pg_stat_force_next_flush() IS NULL AS flushed;
SELECT pg_stat_reset_shared('prolly') IS NULL AS reset;
UPDATE tcc.w SET v = 'busy' WHERE id = 15;
SELECT pg_stat_force_next_flush() IS NULL AS flushed;
INSERT INTO cost SELECT 'busy', name, value FROM pg_stat_prolly;
SELECT pg_stat_force_next_flush() IS NULL AS flushed;
SELECT pg_stat_reset_shared('prolly') IS NULL AS reset;
UPDATE tcc.w SET v = 'busy' WHERE id = 16;
SELECT pg_stat_force_next_flush() IS NULL AS flushed;
INSERT INTO cost SELECT 'busy', name, value FROM pg_stat_prolly;
SELECT c.name, c.value = b.value AS same,
	CASE WHEN c.name NOT LIKE 'fold\_%' THEN c.value > 0 END AS counted
  FROM (SELECT name, min(value) AS value FROM cost WHERE k = 'clean' GROUP BY name) c
  JOIN (SELECT name, min(value) AS value FROM cost WHERE k = 'busy' GROUP BY name) b
 USING (name)
 WHERE c.name IN ('nodes_read', 'nodes_written', 'commits', 'fold_rows_read',
				  'fold_pages', 'fold_candidates', 'fold_full_tables')
 ORDER BY c.name;
SELECT (SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::tcc.w)) AS committed_rows,
	(SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::tcc.o)) AS uncommitted_rows;

-- The rows changed outside the mode wait in the working set through those
-- commits: the baseline each advances still reads them, and a commit with
-- them finds them all.
UPDATE tcc.w SET v = 'busy again' WHERE id = 12;
SELECT (SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::tcc.w)) AS committed_rows,
	(SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::tcc.w)) AS uncommitted_rows;
SET dolt.transaction_commit = off;
SELECT length(dolt_commit('-Am', 'tcc outside')::text) AS committed;
SET dolt.transaction_commit = on;
SELECT (SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::tcc.w)) AS committed_w,
	(SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::tcc.o)) AS committed_o,
	(SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::tcc.w)) AS uncommitted_rows;

-- A row a commit's transaction wrote after its commit (outside the mode)
-- is a change the next commit of a transaction's own rows doesn't hold:
-- it waits in the working set too.  (A subtransaction's aborted xid moves
-- the commit's snapshot past its transaction's.)
SET dolt.transaction_commit = off;
BEGIN;
UPDATE tcc.w SET v = 'before' WHERE id = 20;
SAVEPOINT s;
UPDATE tcc.w SET v = 'gone' WHERE id = 23;
ROLLBACK TO s;
SELECT length(dolt_commit('-am', 'tcc before')::text) AS committed;
UPDATE tcc.w SET v = 'after' WHERE id = 21;
COMMIT;
SET dolt.transaction_commit = on;
UPDATE tcc.w SET v = 'own' WHERE id = 22;
SELECT (SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::tcc.w)) AS committed_rows,
	(SELECT array_agg((to_row).id ORDER BY (to_row).id) FROM dolt_diff('HEAD', 'WORKING', NULL::tcc.w)) AS uncommitted;

\c :home
DROP DATABASE regress_tcc;
