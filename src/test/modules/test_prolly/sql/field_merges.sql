-- A row both sides changed is merged column by column on its encoded
-- fields: a column whose bytes one side left as base's takes the other's,
-- and the merged row is assembled from the fields taken, so deciding the
-- merge decodes no column.  (Writing the merged row to the table decodes
-- what the heap needs.)  The merged rows, conflicts and canonical roots
-- are those of merging decoded values (an assert-enabled build checks
-- every row so).  A row with a column whose base value is unsure (a type
-- change's text and cast readings) is merged decoded, as before.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE FUNCTION pg_temp.counted(name text) RETURNS bigint LANGUAGE sql AS $$
	SELECT value FROM pg_stat_prolly WHERE pg_stat_prolly.name = counted.name
$$;
\set reset 'SELECT pg_stat_force_next_flush(); SELECT pg_stat_reset_shared(''prolly'');'
\set flush 'SELECT pg_stat_force_next_flush();'

CREATE SCHEMA fm VERSIONED;
CREATE TABLE fm.w (id int PRIMARY KEY, a int, b int, c text, d text, e int,
	f int, g int, h int, i int, j int);
INSERT INTO fm.w SELECT g, g, g, 'c' || g, 'd' || g, g, g, g, g, g, g
FROM generate_series(1, 40) g;
SELECT length(dolt_commit('-Am', 'w')::text) AS committed;
SELECT dolt_branch('fm_side');
SELECT dolt_checkout('fm_side') IS NOT NULL AS on_side;
UPDATE fm.w SET b = b + 100, d = d || 'theirs';
UPDATE fm.w SET a = 0 WHERE id = 40;
SELECT length(dolt_commit('-am', 'theirs')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
UPDATE fm.w SET a = a + 100, c = c || 'ours';
SELECT length(dolt_commit('-am', 'ours')::text) AS committed;

-- 39 rows merge, row 40 conflicts (a changed differently); kept, to be
-- counted outside a transaction block
SET dolt.allow_commit_conflicts = on;
:reset
SELECT (dolt_merge('fm_side', '-m', 'merged'))[3:4];
:flush
-- (fewer than one decoding of each version's columns: 3 x 40 x 11)
SELECT pg_temp.counted('columns_decoded') < 3 * 40 * 11 AS decoded_only_to_write;
SELECT count(*) FILTER (WHERE a = id + 100 AND b = id + 100 AND c = 'c' || id || 'ours'
	AND d = 'd' || id || 'theirs') AS merged,
	count(*) AS rows
FROM fm.w;
SELECT a, b, c, d FROM fm.w WHERE id = 40;
SELECT num_conflicts FROM dolt_conflicts;
SELECT (dolt_merge('--abort'))[4];
RESET dolt.allow_commit_conflicts;

-- a column changed alike on both sides, and NULLs set and cleared
SELECT dolt_checkout('fm_side') IS NOT NULL AS on_side;
SELECT dolt_reset('--hard', 'HEAD~1') IS NOT NULL AS reset;
UPDATE fm.w SET e = NULL, f = 7 WHERE id <= 20;
SELECT length(dolt_commit('-am', 'theirs nulls')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_reset('--hard', 'HEAD~1') IS NOT NULL AS reset;
UPDATE fm.w SET f = 7, g = NULL WHERE id <= 20;
SELECT length(dolt_commit('-am', 'ours nulls')::text) AS committed;
SELECT (dolt_merge('fm_side', '-m', 'merged nulls'))[3:4];
SELECT count(*) FILTER (WHERE e IS NULL AND f = 7 AND g IS NULL) AS merged
FROM fm.w WHERE id <= 20;
SELECT count(*) FROM dolt_verify_folds() WHERE NOT matches;

SELECT dolt_branch('-D', 'fm_side');

-- base's 1.5 isn't an integer's text: unsure, so the row merges decoded;
-- theirs' 'yes' against ours' 'true' conflicts, n (2 on both) agrees
CREATE TABLE fm.o (id int PRIMARY KEY, b boolean, n numeric);
INSERT INTO fm.o VALUES (1, true, 1.5);
SELECT length(dolt_commit('-Am', 'o base')::text) AS committed;
SELECT dolt_branch('fm_o');
ALTER TABLE fm.o ALTER COLUMN b TYPE text, ALTER COLUMN n TYPE int;
SELECT length(dolt_commit('-am', 'main: retyped')::text) AS committed;
SELECT dolt_checkout('fm_o') IS NOT NULL AS on_o;
ALTER TABLE fm.o ALTER COLUMN b TYPE text, ALTER COLUMN n TYPE int;
UPDATE fm.o SET b = 'yes' WHERE id = 1;
SELECT length(dolt_commit('-am', 'fm_o: retyped')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
BEGIN;
SELECT (dolt_merge('fm_o', '-m', 'merged retyped'))[2:4];
SELECT * FROM fm.o ORDER BY id;
ROLLBACK;
SELECT dolt_branch('-D', 'fm_o');
DROP SCHEMA fm CASCADE;
SELECT length(dolt_commit('-Am', 'fm gone')::text) AS committed;
