--
-- Version control on the tree engine (D1), through its prolly_* API: diff reports each changed row once, by kind, and a
-- merge without conflicts combines both branches' changes.
--
\set ECHO none
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning;

CREATE TABLE vm (id int PRIMARY KEY, v text);
INSERT INTO vm VALUES (1, 'one'), (3, 'three'), (5, 'five');
SELECT prolly_commit('vm base') AS vm_base \gset
SELECT 'branch', prolly_branch_create('vm_feature', NULL) IS NULL;
SELECT 'checkout feature', prolly_checkout('vm_feature') IS NULL;
INSERT INTO vm VALUES (2, 'two');
UPDATE vm SET v = 'ONE' WHERE id = 1;
DELETE FROM vm WHERE id = 3;
SELECT 'commit feature', prolly_commit('vm feature') IS NOT NULL;

SELECT 'diff base..feature', string_agg(change, ',' ORDER BY change)
FROM prolly_diff(:'vm_base', 'vm_feature') WHERE relation = 'vm'::regclass;
SELECT 'diff feature..base', string_agg(change, ',' ORDER BY change)
FROM prolly_diff('vm_feature', :'vm_base') WHERE relation = 'vm'::regclass;
SELECT 'diff base..base', count(*) FROM prolly_diff(:'vm_base', :'vm_base');

SELECT 'checkout main', prolly_checkout('main') IS NULL;
INSERT INTO vm VALUES (4, 'four');
UPDATE vm SET v = 'FIVE' WHERE id = 5;
SELECT 'commit main', prolly_commit('vm main') IS NOT NULL;
SELECT 'merge', prolly_merge('vm_feature') IS NOT NULL;
SELECT 'merged rows', string_agg(id || ':' || v, ',' ORDER BY id) FROM vm;
SELECT 'diff main..feature', string_agg(change, ',' ORDER BY change)
FROM prolly_diff('main', 'vm_feature') WHERE relation = 'vm'::regclass;
