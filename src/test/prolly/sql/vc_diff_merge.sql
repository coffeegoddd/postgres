--
-- Version control: diff reports each changed row once, by kind, and a
-- merge without conflicts combines both branches' changes.
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

CREATE TABLE vm (id int PRIMARY KEY, v text);
INSERT INTO vm VALUES (1, 'one'), (3, 'three'), (5, 'five');
SELECT (dolt_commit('-Am', 'vm base'))[1] AS vm_base \gset
SELECT 'branch', dolt_branch('vm_feature') IS NOT NULL;
SELECT 'checkout feature', dolt_checkout('vm_feature') IS NOT NULL;
INSERT INTO vm VALUES (2, 'two');
UPDATE vm SET v = 'ONE' WHERE id = 1;
DELETE FROM vm WHERE id = 3;
SELECT 'commit feature', dolt_commit('-Am', 'vm feature') IS NOT NULL;

SELECT 'diff base..feature', string_agg(change, ',' ORDER BY change)
FROM prolly_diff(:'vm_base', 'vm_feature') WHERE relation = 'vm'::regclass;
SELECT 'diff feature..base', string_agg(change, ',' ORDER BY change)
FROM prolly_diff('vm_feature', :'vm_base') WHERE relation = 'vm'::regclass;
SELECT 'diff base..base', count(*) FROM prolly_diff(:'vm_base', :'vm_base');

SELECT 'checkout main', dolt_checkout('main') IS NOT NULL;
INSERT INTO vm VALUES (4, 'four');
UPDATE vm SET v = 'FIVE' WHERE id = 5;
SELECT 'commit main', dolt_commit('-Am', 'vm main') IS NOT NULL;
SELECT 'merge', (dolt_merge('vm_feature'))[3] = '0';
SELECT 'merged rows', string_agg(id || ':' || v, ',' ORDER BY id) FROM vm;
SELECT 'diff main..feature', string_agg(change, ',' ORDER BY change)
FROM prolly_diff('main', 'vm_feature') WHERE relation = 'vm'::regclass;
