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
-- checkouts(): to the other branch and back, and what their folds read;
-- and that the branch is as it should be: clean (nothing in dolt_status)
-- unless changes were left, and folded from its baseline as a full fold
-- has it.
CREATE FUNCTION pg_temp.checkouts(OUT pages int8, OUT candidates int8,
	OUT full_tables int8, OUT changed int8, OUT folds_agree bool) AS $$
BEGIN
	PERFORM test_fold_counters(true);
	PERFORM dolt_checkout('other');
	PERFORM dolt_checkout('main');
	SELECT c.pages, c.candidates, c.full_tables INTO pages, candidates, full_tables
	  FROM test_fold_counters(true) c;
	SELECT count(*) INTO changed FROM dolt_status;
	folds_agree := test_fold_root() = test_fold_root(true);
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

-- a reset to an older commit, its definitions other (a column dropped)
ALTER TABLE co.k ADD COLUMN w int;
SELECT length(dolt_commit('-am', 'w')::text) AS committed;
UPDATE co.k SET v = 'before the reset' WHERE id <= 500;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT 'reset back' AS after, * FROM pg_temp.checkouts();
SELECT 'reset back again' AS after, * FROM pg_temp.checkouts();
SELECT count(*) AS columns FROM information_schema.columns
 WHERE table_schema = 'co' AND table_name = 'k';
-- a merge aborted (a reset to the head, over the merge's rows)
SELECT dolt_checkout('f') IS NOT NULL AS on_f;
UPDATE co.k SET v = 'theirs' WHERE id = 1;
SELECT length(dolt_commit('-am', 'theirs')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
UPDATE co.k SET v = 'ours' WHERE id = 1;
SELECT length(dolt_commit('-am', 'ours')::text) AS committed;
BEGIN;
SELECT (dolt_merge('f'))[2:4];
SELECT (dolt_merge('--abort'))[2:4];
COMMIT;
SELECT 'merge aborted' AS after, * FROM pg_temp.checkouts();
-- a fast-forward over local changes (to another table than it moves)
SELECT dolt_branch('-D', 'f');
SELECT dolt_branch('f');
SELECT dolt_checkout('f') IS NOT NULL AS on_f;
UPDATE co.k SET v = 'g' WHERE id <= 2000;
SELECT length(dolt_commit('-am', 'g')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
UPDATE co.n SET b = 'local' WHERE a <= 10;
SELECT (dolt_merge('f'))[2:4];
SELECT 'ff over local changes' AS after, * FROM pg_temp.checkouts();
SELECT 'ff over local again' AS after, * FROM pg_temp.checkouts();
SELECT count(*) FILTER (WHERE v = 'g') AS moved, (SELECT count(*) FROM co.n WHERE b = 'local') AS local
  FROM co.k;
SELECT dolt_reset('--hard');

DROP SCHEMA co CASCADE;
SELECT length(dolt_commit('-Am', 'co gone')::text) AS committed;
SELECT dolt_branch('-D', 'other', 'f');
