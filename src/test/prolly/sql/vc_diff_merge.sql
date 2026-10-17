--
-- Version control: diff reports each changed row once, by kind, and a
-- merge without conflicts combines both branches' changes.
--
\set ECHO none
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning;

\getenv abs_srcdir PG_ABS_SRCDIR
\set vc_setup :abs_srcdir '/sql/vc_versioned_public.sql'
\i :vc_setup

CREATE TABLE vm (id int PRIMARY KEY, v text);
INSERT INTO vm VALUES (1, 'one'), (3, 'three'), (5, 'five');
SELECT (dolt_commit('-Am', 'vm base'))[1] AS vm_base \gset
SELECT 'branch', (dolt_branch('vm_feature'))[1] = '0';
SELECT 'checkout feature', (dolt_checkout('vm_feature'))[1] = '0';
INSERT INTO vm VALUES (2, 'two');
UPDATE vm SET v = 'ONE' WHERE id = 1;
DELETE FROM vm WHERE id = 3;
SELECT 'commit feature', (dolt_commit('-Am', 'vm feature'))[1] ~ '^[0-9a-f]{64}$';

SELECT 'diff base..feature', string_agg(diff_type, ',' ORDER BY diff_type)
FROM dolt_diff(:'vm_base', 'vm_feature', NULL::vm);
SELECT 'diff feature..base', string_agg(diff_type, ',' ORDER BY diff_type)
FROM dolt_diff('vm_feature', :'vm_base', NULL::vm);
SELECT 'diff base..base', count(*) FROM dolt_diff(:'vm_base', :'vm_base', NULL::vm);

SELECT 'checkout main', (dolt_checkout('main'))[1] = '0';
INSERT INTO vm VALUES (4, 'four');
UPDATE vm SET v = 'FIVE' WHERE id = 5;
SELECT 'commit main', (dolt_commit('-Am', 'vm main'))[1] ~ '^[0-9a-f]{64}$';
SELECT 'merge', (dolt_merge('vm_feature'))[2:4];
SELECT 'merge commit', dolt_hashof('HEAD^2') = dolt_hashof('vm_feature');
SELECT 'merged rows', string_agg(id || ':' || v, ',' ORDER BY id) FROM vm;
SELECT 'diff main..feature', string_agg(diff_type, ',' ORDER BY diff_type)
FROM dolt_diff('main', 'vm_feature', NULL::vm);
