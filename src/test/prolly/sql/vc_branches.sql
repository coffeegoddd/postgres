--
-- Version control: branches isolate data.  Each branch keeps its own
-- working set; checking out another branch shows that branch's rows, and
-- coming back shows this branch's uncommitted changes again.
--
\set ECHO none
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning;

\getenv abs_srcdir PG_ABS_SRCDIR
\set vc_setup :abs_srcdir '/sql/vc_versioned_public.sql'
\i :vc_setup

CREATE TABLE vb (id int PRIMARY KEY, v text);
INSERT INTO vb VALUES (1, 'base');
SELECT (dolt_commit('-Am', 'vb base'))[1] AS vb_base_commit \gset
SELECT 'commit base', :'vb_base_commit' ~ '^[0-9a-f]{64}$';
SELECT 'create branch', (dolt_branch('vb_feature'))[1] = '0';
SELECT 'checkout feature', (dolt_checkout('vb_feature'))[1] = '0';
INSERT INTO vb VALUES (2, 'feature');
UPDATE vb SET v = 'changed' WHERE id = 1;
SELECT 'feature rows', string_agg(id || ':' || v, ',' ORDER BY id) FROM vb;

SELECT 'checkout main', (dolt_checkout('main'))[1] = '0';
SELECT 'main rows', string_agg(id || ':' || v, ',' ORDER BY id) FROM vb;
INSERT INTO vb VALUES (3, 'main');

SELECT 'back to feature', (dolt_checkout('vb_feature'))[1] = '0';
SELECT 'feature rows kept', string_agg(id || ':' || v, ',' ORDER BY id) FROM vb;
SELECT 'commit feature', (dolt_commit('-Am', 'vb feature work'))[1] ~ '^[0-9a-f]{64}$';

SELECT 'checkout main again', (dolt_checkout('main'))[1] = '0';
SELECT 'main rows kept', string_agg(id || ':' || v, ',' ORDER BY id) FROM vb;
SELECT 'commit main', (dolt_commit('-Am', 'vb main work'))[1] ~ '^[0-9a-f]{64}$';

-- A branch can start from a commit: an older one than the head.
SELECT 'branch from commit', (dolt_branch('vb_old', :'vb_base_commit'))[1] = '0';
SELECT 'checkout old', (dolt_checkout('vb_old'))[1] = '0';
SELECT 'old rows', string_agg(id || ':' || v, ',' ORDER BY id) FROM vb;

-- Rolled-back changes leave the branch as it was.
BEGIN;
DELETE FROM vb;
ROLLBACK;
SELECT 'after rollback', count(*) FROM vb;

-- Unknown branches are errors.
SELECT dolt_checkout('vb_no_such_branch');
SELECT 'back to main', (dolt_checkout('main'))[1] = '0';
