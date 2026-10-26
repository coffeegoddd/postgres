-- A branch's first use whose head is a few changes away from the source,
-- those changes spread over a large table: the move planner's estimate of
-- the diff, bounded in the nodes it reads, can't read every leaf they
-- changed, yet takes them for the few they are, so the copy is moved row
-- by row, only the rows that differ written.  It isn't emptied and loaded
-- whole, and so isn't folded in full after: a table loaded with more rows
-- than its change log notes in a transaction has a log that says to read
-- it in full.  Changes bunched together, past an eighth of the rows,
-- still reload.
CREATE DATABASE regress_spread_moves;
-- (the move planner's sources, not pages kept as of the head, which come
-- before them and move nothing: first_use_kept tests those)
ALTER DATABASE regress_spread_moves SET prolly.kept_commits = 0;
\c regress_spread_moves
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
CREATE FUNCTION pg_temp.work() RETURNS TABLE (counter text, grew int8)
LANGUAGE sql AS $$
	SELECT c, pg_temp.since(c)
	FROM unnest(ARRAY['apply_rows', 'apply_loads', 'move_tables_written',
					  'fold_full_log_entry']) c
$$;
CREATE FUNCTION pg_temp.written() RETURNS TABLE (relname name, ins int8, upd int8, del int8)
  LANGUAGE sql AS $$
	SELECT relname, n_tup_ins, n_tup_upd, n_tup_del FROM pg_stat_xact_user_tables
	WHERE schemaname = 'sm' ORDER BY relname
  $$;
CREATE SCHEMA sm VERSIONED;
CREATE TABLE sm.t (id int PRIMARY KEY, v int NOT NULL, p text NOT NULL);
INSERT INTO sm.t SELECT g, 0, repeat(md5(g::text), 3) FROM generate_series(1, 70000) g;
SELECT length(dolt_commit('-Am', 'old')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS old \gset
-- main moved on: a thousand rows, one in every seventy
UPDATE sm.t SET v = 1 WHERE id % 70 = 0;
SELECT length(dolt_commit('-am', 'spread')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS spread \gset

-- The first use of a branch at old: its thousand rows written, nothing
-- loaded, nothing folded in full.
SELECT dolt_branch('back', :'old');
:flush
SELECT count(*) >= 0 AS counting FROM pg_temp.work();
BEGIN;
SELECT dolt_checkout('back');
SELECT * FROM pg_temp.written();
COMMIT;
:flush
SELECT * FROM pg_temp.work();
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_head;
SELECT count(*) FILTER (WHERE v = 0) AS as_old FROM sm.t;
SELECT dolt_checkout('main');

-- Then bunched: twenty-four thousand rows in a row, a third of the table,
-- more leaves than the estimate reads (so it counts every row under those
-- it leaves unread).  A branch at spread, first used from main (back, a
-- thousand rows from spread, gone): emptied and loaded.
SELECT dolt_branch('-D', 'back');
UPDATE sm.t SET v = 2 WHERE id BETWEEN 10001 AND 34000;
SELECT length(dolt_commit('-am', 'bunched')::text) > 0 AS committed;
SELECT dolt_branch('back2', :'spread');
:flush
SELECT count(*) >= 0 AS counting FROM pg_temp.work();
BEGIN;
SELECT dolt_checkout('back2');
SELECT ins = 70000 AND del = 0 AS emptied FROM pg_temp.written();
COMMIT;
:flush
SELECT counter, grew > 0 AS grew FROM pg_temp.work()
WHERE counter IN ('apply_loads', 'move_tables_written');
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_head;
SELECT count(*) FILTER (WHERE v = 1) AS as_spread FROM sm.t;
SELECT dolt_checkout('main');

\c contrib_regression
DROP DATABASE regress_spread_moves;
