-- A fold after another in the same backend, with no catalog change since,
-- works out again only the tables written since and the sequences
-- (fold_incremental): no catalog read through (fold_catalog_scans), the
-- objects described (fold_objects) and tables folded (fold_tables) those
-- alone, however many other versioned objects there are.  Anything else a
-- fold reads changing (a definition, a setting a definition prints by, the
-- branch) folds everything.  debug_prolly_verify_fold_caches, on but where
-- work is counted, folds everything as well and compares.

CREATE DATABASE incremental_canon;
\c incremental_canon
CREATE EXTENSION test_prolly;
\set reset 'SELECT pg_stat_force_next_flush(); SELECT pg_stat_reset_shared(''prolly'');'
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE FUNCTION work() RETURNS TABLE (scans bigint, objects bigint,
	tables bigint, incremental bigint) LANGUAGE sql AS $$
	SELECT max(value) FILTER (WHERE name = 'fold_catalog_scans'),
		max(value) FILTER (WHERE name = 'fold_objects'),
		max(value) FILTER (WHERE name = 'fold_tables'),
		max(value) FILTER (WHERE name = 'fold_incremental')
	FROM pg_stat_prolly
$$;
CREATE FUNCTION objects(schema text, n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..n LOOP
		EXECUTE format('CREATE TABLE %I.t%s (id int PRIMARY KEY, v text)', schema, i);
		EXECUTE format('CREATE FUNCTION %I.f%s() RETURNS int LANGUAGE sql AS ''SELECT %s''', schema, i, i);
		EXECUTE format('CREATE VIEW %I.v%s AS SELECT %s AS x', schema, i, i);
	END LOOP;
END $$;

-- The same row changed with 10 and with 100 of each kind of object
-- besides: the same work.
CREATE SCHEMA few VERSIONED;
CREATE TABLE few.hot (id int PRIMARY KEY, v int);
INSERT INTO few.hot VALUES (1, 0);
CREATE SEQUENCE few.seq;
SELECT objects('few', 10);
SELECT length(dolt_commit('-Am', 'few')::text) > 0 AS committed;
SET debug_prolly_verify_fold_caches = off;
UPDATE few.hot SET v = v + 1;
:reset
SELECT length(dolt_commit('-am', 'few, first')::text) > 0 AS committed;
:flush
SELECT 'few, after another' AS fold, * FROM work();
UPDATE few.hot SET v = v + 1;
:reset
SELECT length(dolt_commit('-am', 'few, one row')::text) > 0 AS committed;
:flush
SELECT 'few, one row' AS fold, * FROM work();
RESET debug_prolly_verify_fold_caches;
DROP SCHEMA few CASCADE;
SELECT length(dolt_commit('-Am', 'few gone')::text) > 0 AS committed;

CREATE SCHEMA many VERSIONED;
CREATE TABLE many.hot (id int PRIMARY KEY, v int);
INSERT INTO many.hot VALUES (1, 0);
CREATE SEQUENCE many.seq;
SELECT objects('many', 100);
SELECT length(dolt_commit('-Am', 'many')::text) > 0 AS committed;
SET debug_prolly_verify_fold_caches = off;
UPDATE many.hot SET v = v + 1;
:reset
SELECT length(dolt_commit('-am', 'many, first')::text) > 0 AS committed;
:flush
SELECT 'many, after another' AS fold, * FROM work();
UPDATE many.hot SET v = v + 1;
:reset
SELECT length(dolt_commit('-am', 'many, one row')::text) > 0 AS committed;
:flush
SELECT 'many, one row' AS fold, * FROM work();
RESET debug_prolly_verify_fold_caches;

-- Each fold below is checked against one of everything.
-- Rows of several tables, and a sequence's value: those worked out again.
UPDATE many.hot SET v = v + 1;
INSERT INTO many.t7 VALUES (1, 'seven');
DELETE FROM many.t7;
INSERT INTO many.t9 VALUES (1, 'nine');
SELECT nextval('many.seq');
:reset
SELECT length(dolt_commit('-am', 'rows')::text) > 0 AS committed;
:flush
SELECT scans, tables, incremental FROM work();
SELECT coalesce(to_table_name, from_table_name) AS table_name, diff_type FROM dolt_diff_summary('HEAD~1', 'HEAD') ORDER BY 1;

-- Only a sequence: its value.
SELECT nextval('many.seq');
SELECT table_name, status FROM dolt_status;
:reset
SELECT length(dolt_commit('-am', 'sequence')::text) > 0 AS committed;
:flush
SELECT scans, tables, incremental FROM work();

-- A working set read between commits: the next commit from it.
UPDATE many.t3 SET v = 'x';
INSERT INTO many.t3 VALUES (3, 'three');
SELECT test_fold_root() IS NOT NULL AS folded;
UPDATE many.t4 SET v = 'y';
INSERT INTO many.t4 VALUES (4, 'four');
:reset
SELECT length(dolt_commit('-am', 'after a read')::text) > 0 AS committed;
:flush
SELECT scans, tables, incremental FROM work();
SELECT coalesce(to_table_name, from_table_name) AS table_name, diff_type FROM dolt_diff_summary('HEAD~1', 'HEAD') ORDER BY 1;

-- A write rolled back to a savepoint, one kept.
BEGIN;
INSERT INTO many.t5 VALUES (5, 'five');
SAVEPOINT s;
INSERT INTO many.t6 VALUES (6, 'six');
ROLLBACK TO SAVEPOINT s;
COMMIT;
:reset
SELECT length(dolt_commit('-am', 'savepoint')::text) > 0 AS committed;
:flush
SELECT scans, incremental FROM work();
SELECT coalesce(to_table_name, from_table_name) AS table_name, diff_type FROM dolt_diff_summary('HEAD~1', 'HEAD') ORDER BY 1;

-- In the writing transaction: its own rows, before and after a fold.
BEGIN;
INSERT INTO many.t8 VALUES (8, 'eight');
SELECT test_fold_root() IS NOT NULL AS folded;
INSERT INTO many.t10 VALUES (10, 'ten');
:reset
SELECT length(dolt_commit('-am', 'own rows')::text) > 0 AS committed;
COMMIT;
:flush
SELECT scans, incremental FROM work();
SELECT coalesce(to_table_name, from_table_name) AS table_name, diff_type FROM dolt_diff_summary('HEAD~1', 'HEAD') ORDER BY 1;

-- Definitions changed: everything.
ALTER TABLE many.t2 ADD COLUMN w int;
UPDATE many.hot SET v = v + 1;
:reset
SELECT length(dolt_commit('-am', 'altered')::text) > 0 AS committed;
:flush
SELECT scans, incremental FROM work();
CREATE OR REPLACE FUNCTION many.f1() RETURNS int LANGUAGE sql AS 'SELECT 100';
:reset
SELECT length(dolt_commit('-am', 'replaced')::text) > 0 AS committed;
:flush
SELECT scans, incremental FROM work();
ALTER TABLE many.t1 RENAME TO t1_renamed;
:reset
SELECT length(dolt_commit('-am', 'renamed')::text) > 0 AS committed;
:flush
SELECT scans, incremental FROM work();
-- (in the transaction that changes them, before they are sent)
BEGIN;
UPDATE many.hot SET v = v + 1;
SELECT test_fold_root() IS NOT NULL AS folded;
CREATE TABLE many.made (id int PRIMARY KEY);
INSERT INTO many.made VALUES (1);
:reset
SELECT length(dolt_commit('-Am', 'made')::text) > 0 AS committed;
COMMIT;
:flush
SELECT scans, incremental FROM work();
SELECT coalesce(to_table_name, from_table_name) AS table_name, diff_type FROM dolt_diff_summary('HEAD~1', 'HEAD') ORDER BY 1;

-- A setting a definition prints by: everything.
UPDATE many.hot SET v = v + 1;
SELECT length(dolt_commit('-am', 'before the setting')::text) > 0 AS committed;
SET DateStyle = 'SQL, DMY';
UPDATE many.hot SET v = v + 1;
:reset
SELECT length(dolt_commit('-am', 'datestyle')::text) > 0 AS committed;
:flush
SELECT scans, incremental FROM work();
RESET DateStyle;

-- Another branch, and back: everything, once.
SELECT dolt_branch('other');
SELECT dolt_checkout('other');
INSERT INTO many.t11 VALUES (11, 'other');
:reset
SELECT length(dolt_commit('-am', 'on other')::text) > 0 AS committed;
:flush
SELECT scans, incremental FROM work();
SELECT dolt_checkout('main');
UPDATE many.hot SET v = v + 1;
:reset
SELECT length(dolt_commit('-am', 'back')::text) > 0 AS committed;
:flush
SELECT scans, incremental FROM work();
SELECT count(*) AS t11_rows FROM many.t11;

-- A fold from no baseline folds everything from nothing, even after one
-- that folded only what changed.
UPDATE many.hot SET v = v + 1;
SELECT test_fold_root() IS NOT NULL AS folded;
:reset
SELECT test_fold_root(true) IS NOT NULL AS folded;
:flush
SELECT scans, incremental FROM work();

-- A fold forced to read everything does.
UPDATE many.hot SET v = v + 1;
SET debug_prolly_force_full_fold = on;
:reset
SELECT length(dolt_commit('-am', 'forced')::text) > 0 AS committed;
:flush
SELECT scans, incremental FROM work();
RESET debug_prolly_force_full_fold;

-- A status reads definitions without rows: the last read's are taken
-- while nothing they're of changed (definitions_reused), no catalog read
-- through, only sequences read again; rows written since don't change
-- them.  It leaves the folds' own items be: the commit after it still
-- works out only what was written.  (verify on: each taken is read again
-- and compared.)
CREATE FUNCTION reused() RETURNS bigint LANGUAGE sql AS $$
	SELECT value FROM pg_stat_prolly WHERE name = 'definitions_reused'
$$;
UPDATE many.hot SET v = v + 1;
SELECT length(dolt_commit('-am', 'before statuses')::text) > 0 AS committed;
UPDATE many.hot SET v = v + 1;
SELECT count(*) AS changes FROM dolt_status;
:reset
INSERT INTO many.t12 VALUES (12, 'twelve');
SELECT * FROM dolt_status ORDER BY 1;
SELECT nextval('many.seq') > 0 AS moved;
:flush
SELECT value AS nodes FROM pg_stat_prolly WHERE name = 'nodes_read' \gset
SET debug_prolly_verify_fold_caches = off;
SELECT * FROM dolt_status ORDER BY 1;
RESET debug_prolly_verify_fold_caches;
:flush
SELECT 'statuses' AS read, scans, reused() FROM work();
-- (each table's changes asked of its baseline entry alone, not the whole
-- baseline read again for each table: ~890 nodes here, ~1200 that way)
SELECT (SELECT value FROM pg_stat_prolly WHERE name = 'nodes_read') - :nodes
	<= 1000 AS status_nodes_bounded;
:reset
SELECT length(dolt_commit('-am', 'after statuses')::text) > 0 AS committed;
:flush
SELECT 'commit after statuses' AS fold, * FROM work();
-- (a definition changed: read again)
ALTER TABLE many.t13 ADD COLUMN w int;
:reset
SELECT * FROM dolt_status ORDER BY 1;
:flush
SELECT 'status after DDL' AS read, scans > 0 AS scanned, reused() FROM work();
SELECT length(dolt_commit('-Am', 'ddl')::text) > 0 AS committed;

SELECT count(*) AS verify_mismatches FROM dolt_verify_folds() WHERE NOT matches;
SELECT count(*) AS changes FROM dolt_status;

\c contrib_regression
DROP DATABASE incremental_canon;
