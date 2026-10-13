--
-- Version control: branches isolate data.  Each branch keeps its own
-- working set; checking out another branch shows that branch's rows, and
-- coming back shows this branch's uncommitted changes again.
--
\set ECHO none
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning;

CREATE TABLE vb (id int PRIMARY KEY, v text);
INSERT INTO vb VALUES (1, 'base');
SELECT 'commit base', prolly_commit('vb base') IS NOT NULL;
SELECT 'create branch', prolly_branch_create('vb_feature', NULL) IS NULL;
SELECT 'checkout feature', prolly_checkout('vb_feature') IS NULL;
INSERT INTO vb VALUES (2, 'feature');
UPDATE vb SET v = 'changed' WHERE id = 1;
SELECT 'feature rows', string_agg(id || ':' || v, ',' ORDER BY id) FROM vb;

SELECT 'checkout main', prolly_checkout('main') IS NULL;
SELECT 'main rows', string_agg(id || ':' || v, ',' ORDER BY id) FROM vb;
INSERT INTO vb VALUES (3, 'main');

SELECT 'back to feature', prolly_checkout('vb_feature') IS NULL;
SELECT 'feature rows kept', string_agg(id || ':' || v, ',' ORDER BY id) FROM vb;
SELECT 'commit feature', prolly_commit('vb feature work') IS NOT NULL;

SELECT 'checkout main again', prolly_checkout('main') IS NULL;
SELECT 'main rows kept', string_agg(id || ':' || v, ',' ORDER BY id) FROM vb;
SELECT prolly_commit('vb main work') AS vb_main_commit \gset

-- A branch can start from a commit.
SELECT 'branch from commit', prolly_branch_create('vb_old', :'vb_main_commit') IS NULL;
SELECT 'checkout old', prolly_checkout('vb_old') IS NULL;
SELECT 'old rows', string_agg(id || ':' || v, ',' ORDER BY id) FROM vb;

-- Rolled-back changes leave the branch as it was.
BEGIN;
DELETE FROM vb;
ROLLBACK;
SELECT 'after rollback', count(*) FROM vb;

-- Unknown branches are errors.
SELECT prolly_checkout('vb_no_such_branch');
SELECT 'back to main', prolly_checkout('main') IS NULL;
