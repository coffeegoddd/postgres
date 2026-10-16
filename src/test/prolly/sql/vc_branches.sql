--
-- Version control: branches isolate data.  Each branch keeps its own
-- working set; checking out another branch shows that branch's rows, and
-- coming back shows this branch's uncommitted changes again.
--
\set ECHO none
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning;

-- Branches hold the tables of versioned schemas: public is one.
SELECT NOT nspversioned AS vc_make_versioned FROM pg_namespace
WHERE nspname = 'public' \gset
\if :vc_make_versioned
DROP SCHEMA public CASCADE;
CREATE SCHEMA public VERSIONED;
\endif

CREATE TABLE vb (id int PRIMARY KEY, v text);
INSERT INTO vb VALUES (1, 'base');
SELECT 'commit base', dolt_commit('-Am', 'vb base') IS NOT NULL;
SELECT 'create branch', dolt_branch('vb_feature') IS NOT NULL;
SELECT 'checkout feature', dolt_checkout('vb_feature') IS NOT NULL;
INSERT INTO vb VALUES (2, 'feature');
UPDATE vb SET v = 'changed' WHERE id = 1;
SELECT 'feature rows', string_agg(id || ':' || v, ',' ORDER BY id) FROM vb;

SELECT 'checkout main', dolt_checkout('main') IS NOT NULL;
SELECT 'main rows', string_agg(id || ':' || v, ',' ORDER BY id) FROM vb;
INSERT INTO vb VALUES (3, 'main');

SELECT 'back to feature', dolt_checkout('vb_feature') IS NOT NULL;
SELECT 'feature rows kept', string_agg(id || ':' || v, ',' ORDER BY id) FROM vb;
SELECT 'commit feature', dolt_commit('-Am', 'vb feature work') IS NOT NULL;

SELECT 'checkout main again', dolt_checkout('main') IS NOT NULL;
SELECT 'main rows kept', string_agg(id || ':' || v, ',' ORDER BY id) FROM vb;
SELECT (dolt_commit('-Am', 'vb main work'))[1] AS vb_main_commit \gset

-- A branch can start from a commit.
SELECT 'branch from commit', dolt_branch('vb_old', :'vb_main_commit') IS NOT NULL;
SELECT 'checkout old', dolt_checkout('vb_old') IS NOT NULL;
SELECT 'old rows', string_agg(id || ':' || v, ',' ORDER BY id) FROM vb;

-- Rolled-back changes leave the branch as it was.
BEGIN;
DELETE FROM vb;
ROLLBACK;
SELECT 'after rollback', count(*) FROM vb;

-- Unknown branches are errors.
SELECT dolt_checkout('vb_no_such_branch');
SELECT 'back to main', dolt_checkout('main') IS NOT NULL;
