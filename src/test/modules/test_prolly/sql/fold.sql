-- Folding a branch's versioned schemas into their canonical form: the root
-- depends only on what they hold, never on how it came to be.  Three
-- branches reach the same tables by different histories (statement order,
-- rows inserted and deleted, columns added and dropped, a column added
-- with a default, objects made and dropped so OIDs differ); a fourth
-- differs in one value.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE SCHEMA app VERSIONED;
SELECT dolt_branch('fa');
SELECT dolt_branch('fb');
SELECT dolt_branch('fc');
\set root 'SELECT test_fold_root() AS root'

-- the long way round
SELECT dolt_checkout('fa');
CREATE TABLE app.junk (x int);
CREATE TYPE app.junk_type AS (a int);
DROP TABLE app.junk;
DROP TYPE app.junk_type;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
INSERT INTO app.t SELECT g, 'tmp' FROM generate_series(1, 200) g;
DELETE FROM app.t WHERE id % 2 = 0;
UPDATE app.t SET v = 'v' || id;
ALTER TABLE app.t ADD COLUMN w int;
UPDATE app.t SET w = id;
ALTER TABLE app.t DROP COLUMN w;
ALTER TABLE app.t ADD COLUMN c int DEFAULT 5;
CREATE INDEX t_v ON app.t (v);
CREATE TABLE app.k (a text);
INSERT INTO app.k VALUES ('x'), ('y'), ('y'), ('z');
DELETE FROM app.k WHERE a = 'z';
CREATE FUNCTION app.f() RETURNS int LANGUAGE sql AS 'SELECT 1';
SELECT test_fold_root() AS fa \gset

-- straight there, in another order
SELECT dolt_checkout('fb');
CREATE FUNCTION app.f() RETURNS int LANGUAGE sql AS 'SELECT 1';
CREATE TABLE app.k (a text);
INSERT INTO app.k VALUES ('y'), ('x'), ('y');
CREATE TABLE app.t (id int PRIMARY KEY, v text, c int DEFAULT 5);
CREATE INDEX t_v ON app.t (v);
INSERT INTO app.t SELECT g, 'v' || g, 5 FROM generate_series(199, 1, -2) g;
-- (names chosen for indexes don't avoid another branch's)
SELECT relname FROM pg_class WHERE relname LIKE 't_pkey%';
SELECT test_fold_root() AS fb \gset

-- the same, after a rewrite and with an ordinary table beside
SELECT dolt_checkout('fc');
CREATE TABLE plain (x int);
INSERT INTO plain VALUES (1);
CREATE FUNCTION app.f() RETURNS int LANGUAGE sql AS 'SELECT 1';
CREATE TABLE app.k (a text);
INSERT INTO app.k VALUES ('y'), ('y'), ('x');
CREATE TABLE app.t (id int PRIMARY KEY, v text, c int DEFAULT 5);
CREATE INDEX t_v ON app.t (v);
INSERT INTO app.t SELECT g, 'v' || g, 5 FROM generate_series(1, 199, 2) g;
VACUUM FULL app.t;
SELECT test_fold_root() AS fc \gset

-- one value differs
SELECT dolt_checkout('main');
CREATE FUNCTION app.f() RETURNS int LANGUAGE sql AS 'SELECT 1';
CREATE TABLE app.k (a text);
INSERT INTO app.k VALUES ('y'), ('y'), ('x');
CREATE TABLE app.t (id int PRIMARY KEY, v text, c int DEFAULT 5);
CREATE INDEX t_v ON app.t (v);
INSERT INTO app.t SELECT g, 'v' || g, CASE WHEN g = 99 THEN 6 ELSE 5 END
  FROM generate_series(1, 199, 2) g;
SELECT test_fold_root() AS fmain \gset

SELECT :'fa' = :'fb' AS a_is_b, :'fb' = :'fc' AS b_is_c, :'fc' <> :'fmain' AS main_differs;
SELECT test_fold_root() = :'fmain' AS stable;

-- A keyless table's duplicate rows count: one copy fewer is another root.
DELETE FROM app.k WHERE ctid = (SELECT min(ctid) FROM app.k WHERE a = 'y');
SELECT test_fold_root() <> :'fmain' AS duplicates_count;

-- Ordinary schemas are every branch's, not the fold's.
DROP TABLE plain;
SELECT dolt_checkout('fc');
SELECT test_fold_root() = :'fc' AS ordinary_tables_left_out;

SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'fa', 'fb', 'fc');
DROP SCHEMA app CASCADE;
