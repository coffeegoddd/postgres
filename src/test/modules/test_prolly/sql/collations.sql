-- A commit records the collations ordering its tables' keys: what decides
-- their order, and their library's version.  A table whose keys are ordered
-- by one that differs now can't have its rows read from that commit, while
-- the commit is still listed and its other tables read.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE SCHEMA app VERSIONED;
CREATE COLLATION app.c_utf8 (provider = builtin, locale = 'C.UTF-8');
CREATE TABLE app.t (k text COLLATE app.c_utf8 PRIMARY KEY, v int);
CREATE TABLE app.c (k text COLLATE "C" PRIMARY KEY);	-- no version
CREATE TABLE app.n (k int PRIMARY KEY, v int);			-- not collatable
INSERT INTO app.t VALUES ('a', 1), ('b', 2);
INSERT INTO app.n VALUES (1, 1);
-- (provider, locale, rules, deterministic, version)
SELECT key, replace(value, E'\n', ' | ') AS value
FROM test_fold_items() WHERE key LIKE 'c:app.%' ORDER BY key;
SELECT length(dolt_commit('-m', 'first')::text) AS committed;
UPDATE app.t SET v = 3 WHERE k = 'a';
UPDATE app.n SET v = 2;
SELECT * FROM dolt_status() ORDER BY table_name;

-- What the catalog recorded of the version doesn't matter, the library's
-- does (a library upgrade leaves the catalog's behind).
SELECT collversion AS saved FROM pg_collation WHERE collname = 'c_utf8' \gset
UPDATE pg_collation SET collversion = 'other' WHERE collname = 'c_utf8';
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::app.t);
UPDATE pg_collation SET collversion = :'saved' WHERE collname = 'c_utf8';

-- The collation's rules change (as recreating it with rules would): the
-- commit's keys of app.t may be in another order; app.n's aren't.
UPDATE pg_collation SET collicurules = '&a<b' WHERE collname = 'c_utf8';
SELECT * FROM dolt_status() ORDER BY table_name;
SELECT * FROM dolt_diff('HEAD', 'WORKING', NULL::app.t);
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::app.n);
UPDATE pg_collation SET collicurules = NULL WHERE collname = 'c_utf8';
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::app.t);
SELECT length(dolt_commit('-am', 'second')::text) AS committed;

-- The collation dropped (with the table it keyed): the commits before are
-- still read, but for that table.
DROP TABLE app.t;
DROP COLLATION app.c_utf8;
UPDATE app.n SET v = 3;
SELECT length(dolt_commit('-am', 'third')::text) AS committed;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~1', 'HEAD', NULL::app.n);
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~2', 'HEAD', NULL::app.n);

DROP SCHEMA app CASCADE;

-- Collations of versioned schemas are held as objects: a move makes them
-- (before the types and tables using them), drops them, and makes one
-- nothing uses again when it changes; one in use is refused.
CREATE SCHEMA mv VERSIONED;
CREATE TABLE mv.base (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'mv')::text) AS committed;
SELECT dolt_branch('mv_none');
CREATE COLLATION mv.c1 FROM "C";
CREATE COLLATION mv.c2 (provider = builtin, locale = 'C.UTF-8');
CREATE COLLATION mv.lone (provider = libc, locale = 'C');
CREATE TABLE mv.t (k text COLLATE mv.c2 PRIMARY KEY, v text COLLATE mv.c1);
INSERT INTO mv.t VALUES ('b', 'x'), ('a', 'y');
SELECT key, replace(value, E'\n', ' | ') AS value
FROM test_fold_items() WHERE key LIKE 'l:mv.%' ORDER BY key;
SELECT length(dolt_commit('-Am', 'collations')::text) AS committed;
SELECT dolt_checkout('mv_none');
SELECT count(*) AS collations FROM pg_collation WHERE collnamespace = 'mv'::regnamespace;
SELECT dolt_reset('--hard', 'main');
SELECT * FROM dolt_status();
SELECT collname, collprovider, collcollate, colllocale FROM pg_collation
  WHERE collnamespace = 'mv'::regnamespace ORDER BY 1;
SELECT k, v FROM mv.t ORDER BY k;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT count(*) AS collations FROM pg_collation WHERE collnamespace = 'mv'::regnamespace;
SELECT dolt_checkout('main');
DROP COLLATION mv.lone;
CREATE COLLATION mv.lone (provider = builtin, locale = 'C');
SELECT length(dolt_commit('-am', 'lone changed')::text) AS committed;
SELECT dolt_checkout('mv_none');
SELECT dolt_reset('--hard', 'main~1');
SELECT dolt_reset('--hard', 'main');
SELECT collname, collprovider FROM pg_collation WHERE collname = 'lone';
SELECT dolt_checkout('main');
DROP TABLE mv.t;
DROP COLLATION mv.c1;
CREATE COLLATION mv.c1 (provider = builtin, locale = 'C');
CREATE TABLE mv.t (k text COLLATE mv.c2 PRIMARY KEY, v text COLLATE mv.c1);
SELECT length(dolt_commit('-Am', 'c1 changed')::text) AS committed;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_reset('--hard', 'HEAD');
SELECT dolt_branch('-D', 'mv_none');
DROP SCHEMA mv CASCADE;
