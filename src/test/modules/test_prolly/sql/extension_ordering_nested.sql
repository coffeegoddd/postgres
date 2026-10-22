-- Polymorphic core comparators can delegate to extension comparators.
SELECT current_database() AS db \gset
CREATE DATABASE regress_ordering_nested;
\c regress_ordering_nested
CREATE SCHEMA tools;
CREATE EXTENSION test_prolly SCHEMA tools;
CREATE EXTENSION test_prolly_ordering VERSION '1.0';
CREATE SCHEMA ord VERSIONED;
CREATE TYPE ord.wrapped AS (k test_ordering);
CREATE TYPE ord.ranged AS RANGE (subtype = test_ordering);
CREATE DOMAIN ord.dom AS test_ordering;
-- Check each dependency path independently, so one cannot mask another.
CREATE TABLE ord.a (k test_ordering[] PRIMARY KEY, n int);
SELECT (dolt_commit('-Am', 'array'))[1] AS a \gset
SELECT replace(d, E'\t', ' | ') FROM unnest(tools.test_commit_dependencies(:'a')) d ORDER BY 1;
DROP TABLE ord.a;
CREATE TABLE ord.c (k ord.wrapped PRIMARY KEY, n int);
SELECT (dolt_commit('-Am', 'record'))[1] AS c \gset
SELECT replace(d, E'\t', ' | ') FROM unnest(tools.test_commit_dependencies(:'c')) d ORDER BY 1;
DROP TABLE ord.c;
CREATE TABLE ord.r (k ord.ranged PRIMARY KEY, n int);
SELECT (dolt_commit('-Am', 'range'))[1] AS r \gset
SELECT replace(d, E'\t', ' | ') FROM unnest(tools.test_commit_dependencies(:'r')) d ORDER BY 1;
DROP TABLE ord.r;
CREATE TABLE ord.d (k ord.dom PRIMARY KEY, n int);
SELECT (dolt_commit('-Am', 'domain'))[1] AS d \gset
SELECT replace(d, E'\t', ' | ') FROM unnest(tools.test_commit_dependencies(:'d')) d ORDER BY 1;
DROP TABLE ord.d;
-- The opclass is user-owned, but its support function still belongs to the extension.
ALTER EXTENSION test_prolly_ordering DROP OPERATOR CLASS test_ordering_ops USING btree;
CREATE TABLE ord.s (k test_ordering PRIMARY KEY, n int);
SELECT (dolt_commit('-Am', 'support function'))[1] AS s \gset
SELECT replace(d, E'\t', ' | ') FROM unnest(tools.test_commit_dependencies(:'s')) d ORDER BY 1;
ALTER EXTENSION test_prolly_ordering ADD OPERATOR CLASS test_ordering_ops USING btree;
CREATE TABLE ord.a (k test_ordering[] PRIMARY KEY, n int, payload text);
CREATE TABLE ord.c (k ord.wrapped PRIMARY KEY, n int);
CREATE TABLE ord.r (k ord.ranged PRIMARY KEY, n int);
CREATE TABLE ord.d (k ord.dom PRIMARY KEY, n int);
CREATE TABLE ord.plain (id int PRIMARY KEY, n int);
INSERT INTO ord.a SELECT ARRAY[ROW(i)::test_ordering], i, repeat('x', 500) FROM generate_series(1, 4000) i;
UPDATE ord.a SET payload = repeat('y', 5000) WHERE n = 4000;
INSERT INTO ord.c SELECT ROW(ROW(i)::test_ordering)::ord.wrapped, i FROM generate_series(1, 200) i;
INSERT INTO ord.r SELECT ord.ranged(ROW(i)::test_ordering, ROW(i)::test_ordering, '[]'), i FROM generate_series(1, 200) i;
INSERT INTO ord.d SELECT ROW(i)::test_ordering, i FROM generate_series(1, 200) i;
INSERT INTO ord.s VALUES (ROW(1), 1), (ROW(2), 2);
INSERT INTO ord.plain VALUES (1, 1);
SELECT (dolt_commit('-Am', 'before upgrade'))[1] AS before \gset
UPDATE ord.s SET n = 11 WHERE (k).v = 1;
SELECT (dolt_commit('-Am', 'to revert'))[1] AS revert_me \gset
UPDATE ord.s SET n = 22 WHERE (k).v = 2;
SELECT dolt_stash('push', 'ord');
SELECT dolt_checkout('-b', 'side');
UPDATE ord.plain SET n = 2;
SELECT length(dolt_commit('-Am', 'side before upgrade')::text) AS committed;
SELECT dolt_checkout('main');
ALTER EXTENSION test_prolly_ordering UPDATE TO '1.1';
REINDEX TABLE ord.a;
REINDEX TABLE ord.c;
REINDEX TABLE ord.r;
REINDEX TABLE ord.d;
REINDEX TABLE ord.s;
SET maintenance_work_mem = '1MB';
-- Unrelated history remains accessible before any post-upgrade commit.
SELECT table_name, status FROM dolt_status() WHERE table_name = 'ord.plain';
SELECT count(*) FROM dolt_diff(:'before', 'WORKING', NULL::ord.plain);
SELECT tools.test_fold_root() = tools.test_fold_root(true) AS same_as_full;
SELECT count(*) AS mismatches FROM dolt_verify_folds() WHERE NOT matches;
-- History is re-sorted, including a sort larger than maintenance_work_mem.
SELECT count(*), sum(n), sum(length(payload)) FROM dolt_at(:'before', NULL::ord.a);
SELECT count(*), sum(n) FROM dolt_at(:'before', NULL::ord.c);
SELECT count(*), sum(n) FROM dolt_at(:'before', NULL::ord.r);
SELECT count(*), sum(n) FROM dolt_at(:'before', NULL::ord.d);
SELECT count(*) FROM dolt_diff(:'before', 'WORKING', NULL::ord.a);
SELECT length(dolt_commit('-Am', 'main after upgrade', '--allow-empty')::text) AS committed;
SELECT dolt_checkout('side');
REINDEX TABLE ord.a;
REINDEX TABLE ord.c;
REINDEX TABLE ord.r;
REINDEX TABLE ord.d;
REINDEX TABLE ord.s;
SELECT length(dolt_commit('-Am', 'side after upgrade', '--allow-empty')::text) AS committed;
SELECT dolt_checkout('main');
-- Both heads now match the installed version; their merge base predates it.
SELECT (dolt_merge('side'))[2:] AS merged;
SELECT * FROM ord.plain;
SELECT dolt_stash('pop', 'ord');
SELECT * FROM ord.s ORDER BY k;
SELECT length(dolt_commit('-Am', 'popped stash')::text) AS committed;
SELECT (dolt_revert(:'revert_me'))[2:] AS reverted;
SELECT * FROM ord.s ORDER BY k;
SELECT count(*) AS mismatches FROM dolt_verify_folds() WHERE NOT matches;
-- Partial checkout carries the source table's ordering metadata with its data.
SELECT dolt_checkout(:'before', 'ord.s');
SELECT * FROM ord.s ORDER BY k;
SELECT dolt_reset('--hard');
-- A comparator that collapses keys refuses only the affected table's rows.
BEGIN;
CREATE OR REPLACE FUNCTION test_ordering_cmp(test_ordering, test_ordering) RETURNS integer
LANGUAGE sql IMMUTABLE STRICT AS 'SELECT 0';
SELECT count(*) FROM dolt_at(:'before', NULL::ord.plain);
SELECT count(*) FROM dolt_at(:'before', NULL::ord.a);
ROLLBACK;
-- A simultaneous schema change cannot reuse a current comparator blindly.
ALTER TABLE ord.a ADD COLUMN extra int;
SELECT count(*) FROM dolt_at(:'before', NULL::ord.a);
SELECT count(*) FROM dolt_at(:'before', NULL::ord.plain);
\c :db
DROP DATABASE regress_ordering_nested;

-- A legacy head has versions but no ordering flags: row-only commit must refold.
CREATE DATABASE regress_ordering_legacy;
\c regress_ordering_legacy
CREATE SCHEMA tools;
CREATE EXTENSION test_prolly SCHEMA tools;
CREATE EXTENSION test_prolly_ordering VERSION '1.0';
CREATE SCHEMA ord VERSIONED;
CREATE TABLE ord.t (k test_ordering[] PRIMARY KEY, n int);
INSERT INTO ord.t SELECT ARRAY[ROW(i)::test_ordering], i FROM generate_series(1, 200) i;
SELECT length(dolt_commit('-Am', 'old comparator')::text) AS committed;
SELECT tools.test_commit_needing(tools.test_fold_root(), ARRAY['extension test_prolly_ordering'],
 ARRAY['{1.0,public}']) AS legacy \gset
SELECT dolt_reset('--soft', :'legacy');
ALTER EXTENSION test_prolly_ordering UPDATE TO '1.1';
REINDEX TABLE ord.t;
SET dolt.transaction_commit = on;
UPDATE ord.t SET n = n + 1 WHERE n = 100;
SET dolt.transaction_commit = off;
SELECT prolly_root_hash('HEAD') = tools.test_fold_root(true) AS committed_full_fold;
SELECT count(*) AS mismatches FROM dolt_verify_folds() WHERE NOT matches;
SELECT replace(d, E'\t', ' | ') FROM unnest(tools.test_commit_dependencies(dolt_hashof('HEAD'))) d ORDER BY 1;
\c :db
DROP DATABASE regress_ordering_legacy;
