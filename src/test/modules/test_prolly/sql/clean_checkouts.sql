-- Leaving a branch with nothing changed reads nothing: a checkout folds
-- the branch to see whether it is clean, and reads only what changed since
-- its baseline.  Moves (a fast-forward, a reset) and the commits of
-- merges and picks leave the baseline at the head, so nothing is.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE SCHEMA co VERSIONED;
CREATE TABLE co.k (id int PRIMARY KEY, v text);
CREATE TABLE co.n (a int, b text);
INSERT INTO co.k SELECT g, 'v' FROM generate_series(1, 20000) g;
INSERT INTO co.n SELECT g, 'v' FROM generate_series(1, 20000) g;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('other');
-- checkouts(): to the other branch and back, and what their folds read.
CREATE FUNCTION pg_temp.checkouts(OUT pages int8, OUT candidates int8, OUT full_tables int8) AS $$
BEGIN
	PERFORM test_fold_counters(true);
	PERFORM dolt_checkout('other');
	PERFORM dolt_checkout('main');
	SELECT c.pages, c.candidates, c.full_tables INTO pages, candidates, full_tables
	  FROM test_fold_counters(true) c;
END $$ LANGUAGE plpgsql;
SELECT 'plain' AS after, * FROM pg_temp.checkouts();
SELECT 'again' AS after, * FROM pg_temp.checkouts();
-- a fast-forward
SELECT dolt_branch('f');
SELECT dolt_checkout('f') IS NOT NULL AS on_f;
UPDATE co.k SET v = 'f' WHERE id <= 2000;
UPDATE co.n SET b = 'f' WHERE a <= 2000;
SELECT length(dolt_commit('-am', 'f')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('f'))[2] AS ff;
SELECT 'ff' AS after, * FROM pg_temp.checkouts();
SELECT 'ff again' AS after, * FROM pg_temp.checkouts();
-- a hard reset of changes
UPDATE co.k SET v = 'r' WHERE id <= 2000;
SELECT dolt_reset('--hard');
SELECT 'reset' AS after, * FROM pg_temp.checkouts();
SELECT 'reset again' AS after, * FROM pg_temp.checkouts();
-- a cherry-pick
SELECT dolt_checkout('f') IS NOT NULL AS on_f;
UPDATE co.k SET v = 'p' WHERE id <= 2000;
UPDATE co.n SET b = 'p' WHERE a <= 2000;
SELECT length(dolt_commit('-am', 'p')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT length(dolt_cherry_pick('f')::text) > 0 AS picked;
SELECT 'pick' AS after, * FROM pg_temp.checkouts();
SELECT 'pick again' AS after, * FROM pg_temp.checkouts();

DROP SCHEMA co CASCADE;
SELECT length(dolt_commit('-Am', 'co gone')::text) AS committed;
SELECT dolt_branch('-D', 'other', 'f');
