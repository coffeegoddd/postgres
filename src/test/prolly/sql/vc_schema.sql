--
-- Version control: schema is versioned with data (the catalogs are prolly
-- tables on the branch).  DDL on
-- a branch is invisible on other branches, including to cached plans and
-- functions, and checking out a branch shows its schema.
--
\set ECHO none
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning;

CREATE TABLE vs (id int PRIMARY KEY, v text);
INSERT INTO vs VALUES (1, 'one');
CREATE FUNCTION vs_row() RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	SELECT vs::text INTO r FROM vs WHERE id = 1;
	RETURN r;
END $$;
PREPARE vs_all AS SELECT * FROM vs ORDER BY id;
SELECT 'commit', prolly_commit('vs base') IS NOT NULL;
SELECT 'branch', prolly_branch_create('vs_feature', NULL) IS NULL;
SELECT 'checkout feature', prolly_checkout('vs_feature') IS NULL;

ALTER TABLE vs ADD COLUMN extra int DEFAULT 7;
ALTER TABLE vs RENAME COLUMN v TO label;
CREATE TABLE vs_only_feature (x int);
INSERT INTO vs_only_feature VALUES (42);
CREATE INDEX vs_label ON vs (label);
SELECT 'feature row', vs::text FROM vs WHERE id = 1;
SELECT 'feature function', vs_row();
EXECUTE vs_all;
SELECT 'feature table', x FROM vs_only_feature;

SELECT 'checkout main', prolly_checkout('main') IS NULL;
SELECT 'main row', vs::text FROM vs WHERE id = 1;
SELECT 'main function', vs_row();
EXECUTE vs_all;
SELECT 'main has no feature table', to_regclass('vs_only_feature') IS NULL;
SELECT 'main has no feature index', to_regclass('vs_label') IS NULL;
SELECT 'main columns', string_agg(attname, ',' ORDER BY attnum)
FROM pg_attribute WHERE attrelid = 'vs'::regclass AND attnum > 0 AND NOT attisdropped;

-- Dropping a table on one branch keeps it on the others.
SELECT 'checkout feature again', prolly_checkout('vs_feature') IS NULL;
DROP TABLE vs_only_feature;
DROP TABLE vs;
SELECT 'feature dropped vs', to_regclass('vs') IS NULL;
SELECT 'checkout main again', prolly_checkout('main') IS NULL;
SELECT 'main still has vs', vs::text FROM vs WHERE id = 1;
DEALLOCATE vs_all;
