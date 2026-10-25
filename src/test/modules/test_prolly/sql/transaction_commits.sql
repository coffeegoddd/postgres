-- dolt.transaction_commit: a Dolt commit of every SQL transaction that
-- changes a branch, made as it commits, and undone with it.
CREATE SCHEMA tc VERSIONED;
CREATE TABLE tc.t (id int PRIMARY KEY, v text);
CREATE TABLE tc_plain (id int);
SELECT length(dolt_commit('-Am', 'tc base')::text) AS committed;
CREATE FUNCTION pg_temp.commits() RETURNS bigint LANGUAGE sql
  AS $$ SELECT count(*) FROM dolt_log('main') $$;
SELECT pg_temp.commits() AS before \gset
SET dolt.transaction_commit = on;

-- A statement of its own (autocommit) commits; so does a block, once.
INSERT INTO tc.t VALUES (1, 'a');
SELECT pg_temp.commits() - :before AS commits,
	(SELECT message FROM dolt_log('main') LIMIT 1) AS message;
BEGIN;
INSERT INTO tc.t VALUES (2, 'b');
UPDATE tc.t SET v = 'B' WHERE id = 2;
COMMIT;
SELECT pg_temp.commits() - :before AS commits;
SELECT name, dirty FROM dolt_branches() WHERE name = 'main';

-- What doesn't commit makes none: a rollback, a savepoint rolled back, an
-- error caught, an error at commit (a deferred constraint).
BEGIN;
INSERT INTO tc.t VALUES (3, 'c');
ROLLBACK;
BEGIN;
SAVEPOINT s;
INSERT INTO tc.t VALUES (3, 'c');
ROLLBACK TO s;
COMMIT;
DO $$
BEGIN
	INSERT INTO tc.t VALUES (3, 'c');
	RAISE EXCEPTION 'undo';
EXCEPTION WHEN raise_exception THEN
	NULL;
END $$;
CREATE TABLE tc.child (id int PRIMARY KEY,
	parent int REFERENCES tc.t DEFERRABLE INITIALLY DEFERRED);
SELECT pg_temp.commits() AS before \gset
BEGIN;
INSERT INTO tc.child VALUES (1, 99);
COMMIT;
SELECT pg_temp.commits() - :before AS commits;

-- Nor does a transaction that changes nothing of a branch: reads,
-- unversioned writes, and a row inserted and deleted again.
SELECT count(*) FROM tc.t;
INSERT INTO tc_plain VALUES (1);
BEGIN;
INSERT INTO tc.t VALUES (4, 'gone');
DELETE FROM tc.t WHERE id = 4;
COMMIT;
SELECT pg_temp.commits() - :before AS commits;

-- Definitions alone commit; the message is the setting's at COMMIT.
BEGIN;
CREATE TABLE tc.u (id int);
SET LOCAL dolt.transaction_commit_message = 'made u';
COMMIT;
SELECT pg_temp.commits() - :before AS commits,
	(SELECT message FROM dolt_log('main') LIMIT 1) AS message;

-- (a commit's message isn't empty)
SET dolt.transaction_commit_message = '';

-- Owned by its writer, a branch a transaction leaves clean (nothing to
-- commit) is given up too.
SELECT dolt_writer_mode('owned');
SET dolt.writer = 'tc_writer';
BEGIN;
INSERT INTO tc.t VALUES (40, 'gone');
DELETE FROM tc.t WHERE id = 40;
COMMIT;
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';
SELECT dolt_writer_mode('shared');
RESET dolt.writer;

-- A dolt_commit() in the transaction commits what came before it; the
-- transaction's commit holds only what came after, if anything did.
SELECT pg_temp.commits() AS before \gset
BEGIN;
INSERT INTO tc.t VALUES (5, 'e');
SELECT length(dolt_commit('-m', 'mine')::text) AS committed;
COMMIT;
BEGIN;
INSERT INTO tc.t VALUES (6, 'f');
SELECT length(dolt_commit('-m', 'mine again')::text) AS committed;
INSERT INTO tc.t VALUES (7, 'g');
COMMIT;
SELECT pg_temp.commits() - :before AS commits;
SELECT message FROM dolt_log('main') LIMIT 3;

-- Two branches changed, or one left, can't commit so.
SELECT dolt_branch('tc_f');
BEGIN;
INSERT INTO tc.t VALUES (8, 'main');
SELECT dolt_checkout('tc_f');
INSERT INTO tc.t VALUES (8, 'f');
COMMIT;
SELECT active_branch();
BEGIN;
SELECT dolt_checkout('tc_f');
INSERT INTO tc.t VALUES (8, 'f');
SELECT dolt_checkout('main');
COMMIT;
SELECT dolt_checkout('main');

-- A branch committed by a version-control call is that call's: integrating
-- one's own branch makes its commits, and no other.
SELECT pg_temp.commits() AS before \gset
BEGIN;
SELECT dolt_checkout('tc_f');
INSERT INTO tc.t VALUES (9, 'f');
SELECT (dolt_integrate('main', '-m', 'f in'))[2] AS fast_forward;
COMMIT;
SELECT dolt_checkout('main');
SELECT pg_temp.commits() - :before AS commits;

-- Conflicts kept (dolt.allow_commit_conflicts): the transaction commits,
-- without a Dolt commit.
SELECT dolt_checkout('tc_f');
UPDATE tc.t SET v = 'on f' WHERE id = 1;
SELECT dolt_checkout('main');
UPDATE tc.t SET v = 'on main' WHERE id = 1;
SELECT pg_temp.commits() AS before \gset
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('tc_f'))[3] AS conflicts;
RESET dolt.allow_commit_conflicts;
SELECT pg_temp.commits() - :before AS commits;
SELECT dolt_merge('--abort') IS NOT NULL AS aborted;

-- A transaction's commit is its own row changes on the head: nothing is
-- folded, and rows another writer committed outside the mode (in the
-- working set, not yet in a commit) aren't in it.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT length(dolt_commit('-Am', 'tc clean', '--allow-empty')::text) AS committed;
SET dolt.transaction_commit = off;
INSERT INTO tc.t VALUES (20, 'outside the mode');
SET dolt.transaction_commit = on;
SELECT test_fold_counters(true) IS NOT NULL AS reset;
INSERT INTO tc.t VALUES (21, 'mine');
UPDATE tc.t SET v = 'mine, updated' WHERE id = 21;
SELECT pages, candidates, full_tables FROM test_fold_counters();
BEGIN;
INSERT INTO tc.t VALUES (22, 'kept');
SAVEPOINT s;
INSERT INTO tc.t VALUES (23, 'rolled back');
ROLLBACK TO s;
SAVEPOINT r;
UPDATE tc.t SET v = 'kept, released' WHERE id = 22;
RELEASE r;
DELETE FROM tc.t WHERE id = 1;
COMMIT;
SELECT pages, candidates, full_tables FROM test_fold_counters();
SELECT name, dirty FROM dolt_branches() WHERE name = 'main';
-- (the head has the mode's rows; the working set, the other writer's too)
SET dolt.transaction_commit = off;
SELECT dolt_reset('--hard');
SELECT id, v FROM tc.t WHERE id IN (1, 20, 21, 22, 23) ORDER BY id;
SET dolt.transaction_commit = on;

-- A table without a key: its rows' copies counted.
CREATE TABLE tc.bag (v text);
SELECT test_fold_counters(true) IS NOT NULL AS reset;
INSERT INTO tc.bag VALUES ('a'), ('a'), ('b');
DELETE FROM tc.bag WHERE ctid = (SELECT min(ctid) FROM tc.bag WHERE v = 'a');
UPDATE tc.bag SET v = 'c' WHERE v = 'b';
SELECT pages, candidates, full_tables FROM test_fold_counters();
SELECT name, dirty FROM dolt_branches() WHERE name = 'main';
SELECT v, count(*) FROM tc.bag GROUP BY v ORDER BY v;

-- What can't be committed so is committed as the working set: definitions
-- changed, a table the head doesn't have, a keyless row the head never had.
BEGIN;
ALTER TABLE tc.bag ADD COLUMN n int;
INSERT INTO tc.bag VALUES ('d', 1);
COMMIT;
SELECT name, dirty FROM dolt_branches() WHERE name = 'main';
SET dolt.transaction_commit = off;
CREATE TABLE tc.later (id int PRIMARY KEY);
INSERT INTO tc.bag VALUES ('outside', 2);
SET dolt.transaction_commit = on;
INSERT INTO tc.later VALUES (1);
SELECT name, dirty FROM dolt_branches() WHERE name = 'main';
SET dolt.transaction_commit = off;
INSERT INTO tc.bag VALUES ('outside again', 3);
SET dolt.transaction_commit = on;
DELETE FROM tc.bag WHERE v = 'outside again';
SELECT name, dirty FROM dolt_branches() WHERE name = 'main';

-- A dolt_commit() undone by a rollback to a savepoint: what came before it
-- is the transaction's again.
BEGIN;
INSERT INTO tc.t VALUES (50, 'before');
SAVEPOINT s;
SELECT length(dolt_commit('-Am', 'undone')::text) AS committed;
ROLLBACK TO s;
INSERT INTO tc.t VALUES (51, 'after');
COMMIT;
SELECT name, dirty FROM dolt_branches() WHERE name = 'main';

-- Two tables' names swapped by a writer outside the mode: rows written to
-- one aren't committed to the other.
CREATE TABLE tc.sa (id int, v text);
CREATE TABLE tc.sb (id int, v text);
INSERT INTO tc.sa VALUES (100, 'sa');
INSERT INTO tc.sb VALUES (200, 'sb');
SET dolt.transaction_commit = off;
ALTER TABLE tc.sa RENAME TO sx;
ALTER TABLE tc.sb RENAME TO sa;
ALTER TABLE tc.sx RENAME TO sb;
SET dolt.transaction_commit = on;
INSERT INTO tc.sa VALUES (1, 'was sb');
SELECT name, dirty FROM dolt_branches() WHERE name = 'main';

-- Keys equal to the keying but not in their bytes (numeric's scale).
CREATE TABLE tc.num (id numeric PRIMARY KEY);
CREATE TABLE tc.nbag (v numeric);
INSERT INTO tc.num VALUES (1.0);
INSERT INTO tc.nbag VALUES (1.0), (1.0);
UPDATE tc.num SET id = 1.00 WHERE id = 1.0;
INSERT INTO tc.nbag VALUES (1.00);
SELECT name, dirty FROM dolt_branches() WHERE name = 'main';
SELECT id::text FROM tc.num;
SELECT v::text, count(*) FROM tc.nbag GROUP BY v::text ORDER BY 1;

-- The most row changes counts the versions made: one insert is one.
SET dolt.transaction_commit = off;
INSERT INTO tc.t VALUES (60, 'outside');
SET dolt.transaction_commit = on;
SET dolt.transaction_commit_max_changes = 1;
INSERT INTO tc.t VALUES (61, 'own');
SELECT name, dirty FROM dolt_branches() WHERE name = 'main';
RESET dolt.transaction_commit_max_changes;
SET dolt.transaction_commit = off;
SELECT length(dolt_commit('-Am', 'outside')::text) AS committed;
SET dolt.transaction_commit = on;

-- The chunks a transaction's commit writes go into its backend's chunk
-- cache as it commits: the next transaction's commit, reading the head
-- the first made (its commit, root, the table's path and the refs), reads
-- none of them from the chunk store's files.
CREATE TABLE tc.cached (id int PRIMARY KEY, v int);
INSERT INTO tc.cached SELECT g, 0 FROM generate_series(1, 20) g;
UPDATE tc.cached SET v = 1 WHERE id = 1;
SELECT pg_stat_force_next_flush();
SELECT value AS file_reads FROM pg_stat_prolly WHERE name = 'chunk_file_reads' \gset
UPDATE tc.cached SET v = 1 WHERE id = 2;
SELECT pg_stat_force_next_flush();
SELECT value - :file_reads AS file_reads FROM pg_stat_prolly
  WHERE name = 'chunk_file_reads';

-- What a commit lets into the cache costs nothing per subtransaction: a
-- commit then many subtransactions in one transaction keep the backend's
-- memory as it was.
SET dolt.transaction_commit = off;
DO $$
DECLARE
	before bigint;
	after bigint;
BEGIN
	PERFORM dolt_commit('-Am', 'before subtransactions', '--allow-empty');
	SELECT sum(total_bytes) INTO before FROM pg_backend_memory_contexts;
	FOR i IN 1..20000 LOOP
		BEGIN
			PERFORM 1;
		EXCEPTION WHEN OTHERS THEN NULL;
		END;
	END LOOP;
	SELECT sum(total_bytes) INTO after FROM pg_backend_memory_contexts;
	RAISE NOTICE 'memory bounded: %', after - before < 8 * 1024 * 1024;
END $$;
SET dolt.transaction_commit = on;

-- A small commit reads each node it needs once, however often it descends
-- it: the refs' root for each entry it gets and puts (its record of the
-- relations it was made from, a new entry, among them), the head's commit,
-- the root's and the table's upper nodes for each row's seek and then the
-- apply, each level of which starts from the root.  Three rows of a table
-- three levels high read about their paths, not a descent per use.  (This
-- backend's counts, not the database's, which other processes' work
-- moves.  The tables' trees' nodes, exactly as many each time, apart from
-- the refs' and commits' trees' (refs_nodes_read): their leaves hold hashes
-- and dates, so where their chunks split, and how many leaves the same
-- entries span, moves from run to run.  Those are bounded loosely, against
-- a read of the refs whole.)
CREATE TABLE tc.tall (id int PRIMARY KEY, v int, pad text);
INSERT INTO tc.tall SELECT g, 0, md5(g::text) FROM generate_series(1, 20000) g;
UPDATE tc.tall SET v = 1 WHERE id IN (1, 10000, 19999);
SELECT pg_stat_force_next_flush();
SELECT value AS nodes FROM test_prolly_session_stats() WHERE name = 'nodes_read' \gset
SELECT value AS refs FROM test_prolly_session_stats() WHERE name = 'refs_nodes_read' \gset
UPDATE tc.tall SET v = 2 WHERE id IN (2, 10001, 20000);
SELECT pg_stat_force_next_flush();
SELECT value - :nodes AS nodes_read FROM test_prolly_session_stats()
  WHERE name = 'nodes_read' \gset
SELECT value - :refs AS refs_read FROM test_prolly_session_stats()
  WHERE name = 'refs_nodes_read' \gset
SELECT CASE WHEN :nodes_read - :refs_read <= 12 THEN 'at most 12'
		ELSE (:nodes_read - :refs_read)::text END AS tables_read,
	CASE WHEN :refs_read <= 60 THEN 'at most 60' ELSE :refs_read::text END
	AS refs_read;

-- A hash that names a tree but no commit (here, a commit's root, of many
-- items) is found not to be one from a descent of that tree, not a read
-- of all of it.  (Its nodes and the refs' count as the refs' and commits'
-- trees' (refs_nodes_read), whose chunks split where hashes and dates fall:
-- a leaf or two either way, against a read of the whole root.)
DO $$
BEGIN
	FOR i IN 1..150 LOOP
		EXECUTE format('CREATE TABLE tc.wide%s (id int PRIMARY KEY, %s)', i,
			(SELECT string_agg(format('column_with_a_long_name_%s text', c), ', ')
			 FROM generate_series(1, 12) c));
	END LOOP;
END $$;
SELECT length(dolt_commit('-Am', 'wide')::text) > 0 AS committed;
CREATE FUNCTION pg_temp.names_commit(h text) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	PERFORM dolt_hashof(h);
	RETURN true;
EXCEPTION WHEN OTHERS THEN
	RETURN false;
END $$;
SELECT root AS wide_root FROM test_commit_roots('HEAD') \gset
SELECT pg_stat_force_next_flush();
SELECT value AS nodes FROM test_prolly_session_stats() WHERE name = 'nodes_read' \gset
SELECT pg_temp.names_commit(:'wide_root') AS names_commit;
SELECT pg_stat_force_next_flush();
SELECT value - :nodes AS nodes_read FROM test_prolly_session_stats()
  WHERE name = 'nodes_read' \gset
SELECT CASE WHEN :nodes_read <= 10 THEN 'at most 10' ELSE :nodes_read::text END
  AS nodes_read;
DO $$
BEGIN
	FOR i IN 1..150 LOOP
		EXECUTE format('DROP TABLE tc.wide%s', i);
	END LOOP;
END $$;

-- Off again, writes wait for dolt_commit().
RESET dolt.transaction_commit;
INSERT INTO tc.t VALUES (10, 'later');
SELECT pg_temp.commits() - :before AS commits;
SELECT dolt_reset('--hard');
SELECT dolt_branch('-D', 'tc_f');
DROP TABLE tc_plain;
DROP SCHEMA tc CASCADE;
SELECT length(dolt_commit('-Am', 'tc gone')::text) AS committed;
