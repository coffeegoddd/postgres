-- dolt.transaction_commit advances the baseline with each transaction's
-- commit of its own rows, where the baseline held the old head's: so a
-- fold after many transactions (a status, the owner's release check in an
-- owned writer mode) reads the changes since the last commit, not every
-- one since the last fold.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT current_database() AS home \gset
CREATE DATABASE txn_commit_baselines;
\c txn_commit_baselines
CREATE EXTENSION test_prolly;
CREATE SCHEMA tb VERSIONED;
CREATE TABLE tb.k (id int PRIMARY KEY, v text);
CREATE TABLE tb.n (v int);
INSERT INTO tb.k SELECT g, 'k' FROM generate_series(1, 1000) g;
INSERT INTO tb.n SELECT g % 10 FROM generate_series(1, 1000) g;
SELECT length(dolt_commit('-Am', 'base')::text) > 0 AS committed;
CREATE FUNCTION pg_temp.counter(n text) RETURNS bigint LANGUAGE sql
  AS $$ SELECT value FROM pg_stat_prolly WHERE name = n $$;

SET dolt.transaction_commit = on;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');

-- 40 one-row transactions on each table (each statement its own), then
-- a status: its fold reads what no commit holds (nothing), not the 80
-- transactions' rows; and the baseline is the fold's in full.
SELECT format('UPDATE tb.k SET v = %L WHERE id = %s', 'x' || g, g)
FROM generate_series(1, 40) g \gexec
SELECT format('INSERT INTO tb.n VALUES (%s)', g) FROM generate_series(1, 40) g \gexec
SELECT pg_stat_force_next_flush();
SELECT pg_temp.counter('commits') AS commits;
SELECT pg_stat_reset_shared('prolly');
SELECT count(*) AS changed FROM dolt_status;
SELECT pg_stat_force_next_flush();
SELECT pg_temp.counter('fold_rows_read') AS status_rows_read,
	pg_temp.counter('fold_full_tables') AS status_full_tables;
SELECT test_fold_root() = test_fold_root(true) AS same_as_full;
SELECT count(*), count(*) FILTER (WHERE v LIKE 'x%') FROM tb.k;
SELECT count(*), sum(v) FROM tb.n;

-- In an owned writer mode, each transaction's release of the branch's
-- owner checks the working set is the head's, folding it: each reads the
-- changes since the last commit, not since the last fold (else the 40th
-- reads all 40 transactions' rows, 820 in all).
SELECT dolt_writer_mode('owned');
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT format('UPDATE tb.k SET v = %L WHERE id = %s', 'y' || g, g)
FROM generate_series(1, 40) g \gexec
SELECT pg_stat_force_next_flush();
SELECT pg_temp.counter('fold_rows_read') <= 40 AS owner_checks_read_little,
	pg_temp.counter('fold_full_tables') AS owner_full_tables;
SELECT name, dirty FROM dolt_branches() WHERE name = 'main';
SELECT test_fold_root() = test_fold_root(true) AS same_as_full;
SELECT dolt_writer_mode('shared');

-- A transaction that changes the table and rolls a part back: only what
-- it kept is committed, and folded.
BEGIN;
UPDATE tb.k SET v = 'kept' WHERE id = 100;
SAVEPOINT s;
UPDATE tb.k SET v = 'undone' WHERE id = 101;
INSERT INTO tb.n VALUES (77);
ROLLBACK TO s;
INSERT INTO tb.n VALUES (88);
COMMIT;
SELECT count(*) AS changed FROM dolt_status;
SELECT test_fold_root() = test_fold_root(true) AS same_as_full;
SELECT v FROM tb.k WHERE id IN (100, 101) ORDER BY id;
SELECT count(*) FILTER (WHERE v = 77) AS undone, count(*) FILTER (WHERE v = 88) AS kept
FROM tb.n;

-- A working-set commit after (a DDL transaction's) folds from the
-- advanced baseline: incrementally.
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
BEGIN;
UPDATE tb.k SET v = 'ddl' WHERE id = 1;
CREATE TABLE tb.extra (id int PRIMARY KEY);
COMMIT;
SELECT pg_stat_force_next_flush();
SELECT pg_temp.counter('fold_full_tables') AS full_tables_new_table_only;
SELECT test_fold_root() = test_fold_root(true) AS same_as_full;
RESET dolt.transaction_commit;

\c :home
DROP DATABASE txn_commit_baselines;
