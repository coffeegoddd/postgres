--
-- Version control: schema is versioned with data (each branch has its own
-- copy of a versioned schema's objects).  DDL on a branch is invisible on
-- other branches, including to cached plans and functions, and checking
-- out a branch shows its schema.
--
\set ECHO none
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning;

\getenv abs_srcdir PG_ABS_SRCDIR
\set vc_setup :abs_srcdir '/sql/vc_versioned_public.sql'
\i :vc_setup

CREATE TABLE vs (id int PRIMARY KEY, v text);
INSERT INTO vs VALUES (1, 'one');
CREATE FUNCTION vs_row() RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	SELECT vs::text INTO r FROM vs WHERE id = 1;
	RETURN r;
END $$;
PREPARE vs_all AS SELECT * FROM vs ORDER BY id;
SELECT 'commit', length((dolt_commit('-Am', 'vs base'))[1]) = 40;
SELECT 'branch', (dolt_branch('vs_feature'))[1] = '0';
SELECT 'checkout feature', (dolt_checkout('vs_feature'))[1] = '0';

ALTER TABLE vs ADD COLUMN extra int DEFAULT 7;
ALTER TABLE vs RENAME COLUMN v TO label;
CREATE TABLE vs_only_feature (x int);
INSERT INTO vs_only_feature VALUES (42);
CREATE INDEX vs_label ON vs (label);
SELECT 'feature row', vs::text FROM vs WHERE id = 1;
SELECT 'feature function', vs_row();
-- (as in Postgres: its result columns changed on this branch)
EXECUTE vs_all;
-- prepared again here, it reads this branch's table
PREPARE vs_feature_all AS SELECT * FROM vs ORDER BY id;
EXECUTE vs_feature_all;
DEALLOCATE vs_feature_all;
SELECT 'feature table', x FROM vs_only_feature;

SELECT 'checkout main', (dolt_checkout('main'))[1] = '0';
SELECT 'main row', vs::text FROM vs WHERE id = 1;
SELECT 'main function', vs_row();
EXECUTE vs_all;
SELECT 'main has no feature table', to_regclass('vs_only_feature') IS NULL;
SELECT 'main has no feature index', to_regclass('vs_label') IS NULL;
SELECT 'main columns', string_agg(attname, ',' ORDER BY attnum)
FROM pg_attribute WHERE attrelid = 'vs'::regclass AND attnum > 0 AND NOT attisdropped;

-- Dropping a table on one branch keeps it on the others.
SELECT 'checkout feature again', (dolt_checkout('vs_feature'))[1] = '0';
DROP TABLE vs_only_feature;
DROP TABLE vs;
SELECT 'feature dropped vs', to_regclass('vs') IS NULL;
SELECT 'checkout main again', (dolt_checkout('main'))[1] = '0';
SELECT 'main still has vs', vs::text FROM vs WHERE id = 1;
DEALLOCATE vs_all;
