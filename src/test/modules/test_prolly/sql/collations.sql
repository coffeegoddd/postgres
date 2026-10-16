-- A commit records the versions of the collations ordering its tables'
-- keys, and isn't read where they differ.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE SCHEMA app VERSIONED;
CREATE COLLATION app.c_utf8 (provider = builtin, locale = 'C.UTF-8');
CREATE TABLE app.t (k text COLLATE app.c_utf8 PRIMARY KEY, v int);
CREATE TABLE app.c (k text COLLATE "C" PRIMARY KEY);	-- no version
CREATE TABLE app.n (k int PRIMARY KEY);				-- not collatable
INSERT INTO app.t VALUES ('a', 1), ('b', 2);
SELECT key, replace(value, E'\n', ' | ') AS value
FROM test_fold_items() WHERE key LIKE 'c:app.%' ORDER BY key;
SELECT length(dolt_commit('-m', 'first')::text) AS committed;
UPDATE app.t SET v = 3 WHERE k = 'a';
SELECT * FROM dolt_status();

-- The collation's version changes: the commit's keys may be in another
-- order now.
SELECT collversion AS saved FROM pg_collation WHERE collname = 'c_utf8' \gset
UPDATE pg_collation SET collversion = 'other' WHERE collname = 'c_utf8';
SELECT * FROM dolt_status();
SELECT * FROM dolt_diff('HEAD', 'WORKING', NULL::app.t);
UPDATE pg_collation SET collversion = :'saved' WHERE collname = 'c_utf8';
SELECT * FROM dolt_status();
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::app.t);

SELECT length(dolt_commit('-m', 'second')::text) AS committed;
DROP SCHEMA app CASCADE;
