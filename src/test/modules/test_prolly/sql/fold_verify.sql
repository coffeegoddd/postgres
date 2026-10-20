-- The verifier folds each table from its baseline and in full by one
-- snapshot and compares the trees: clean after every kind of change; a
-- baseline whose changes went missing is caught, counted, and its table
-- read in full at its next fold, which then matches.
CREATE DATABASE regress_fold_verify;
\c regress_fold_verify
CREATE EXTENSION test_prolly;
\set reset 'SELECT pg_stat_force_next_flush(); SELECT pg_stat_reset_shared(''prolly'');'
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE FUNCTION verified() RETURNS TABLE (name text, value bigint) LANGUAGE sql AS $$
	SELECT name, value FROM pg_stat_prolly
	WHERE name IN ('fold_verified', 'fold_verify_mismatches', 'fold_full_verify',
				   'fold_full_forced')
	ORDER BY name
$$;
CREATE SCHEMA fv VERSIONED;
CREATE TABLE fv.k (id int PRIMARY KEY, v text);
CREATE TABLE fv.kl (a int, b text);
INSERT INTO fv.k SELECT g, 'v' || g FROM generate_series(1, 2000) g;
INSERT INTO fv.kl SELECT g % 10, 'b' FROM generate_series(1, 200) g;
SELECT length(dolt_commit('-Am', 'loaded')::text) AS committed;

-- changes since the baseline, of every kind: clean
UPDATE fv.k SET v = 'changed' WHERE id % 100 = 0;
DELETE FROM fv.k WHERE id > 1990;
INSERT INTO fv.k VALUES (5000, 'new');
DELETE FROM fv.kl WHERE a = 3;
INSERT INTO fv.kl VALUES (3, 'again'), (11, 'b');
:reset
SELECT * FROM dolt_verify_folds() ORDER BY 1;
:flush
SELECT * FROM verified();

-- committed, and more changes: still clean; a sample of none checks none
SELECT length(dolt_commit('-am', 'changed')::text) AS committed;
UPDATE fv.k SET v = 'again' WHERE id < 10;
SELECT * FROM dolt_verify_folds() ORDER BY 1;
SELECT count(*) FROM dolt_verify_folds(0);
SELECT * FROM dolt_verify_folds(2);

-- a table's changes lost from its log, its baseline kept: caught, counted,
-- and its next fold reads it in full, which matches again
UPDATE fv.k SET v = 'lost' WHERE id BETWEEN 100 AND 150;
SELECT test_lose_change_log('fv.k');
:reset
\set VERBOSITY terse
SELECT * FROM dolt_verify_folds() ORDER BY 1;
\set VERBOSITY default
:flush
SELECT * FROM verified();
SELECT * FROM dolt_verify_folds() ORDER BY 1;
SELECT count(*) AS lost_rows_seen FROM dolt_diff('HEAD', 'WORKING', NULL::fv.k)
WHERE (to_row).v = 'lost';

\c contrib_regression
DROP DATABASE regress_fold_verify;
