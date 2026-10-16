-- dolt_status and dolt_diff of branches made by copying: versions compared
-- as their folded trees.

CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
CREATE TABLE app.gone (x int);
CREATE TABLE app.k (a text);
INSERT INTO app.t SELECT g, 'v' || g FROM generate_series(1, 2000) g;
INSERT INTO app.k VALUES ('x'), ('y'), ('y');
INSERT INTO app.gone VALUES (1);
SELECT length(dolt_commit('-m', 'first')::text) AS committed;
SELECT * FROM dolt_status();

-- Rows changed, a table made, one dropped, one redefined.
UPDATE app.t SET v = 'changed' WHERE id IN (7, 1500);
DELETE FROM app.t WHERE id = 3;
INSERT INTO app.t VALUES (2001, 'new');
CREATE TABLE app.made (y int);
DROP TABLE app.gone;
ALTER TABLE app.k ADD COLUMN b int;
SELECT * FROM dolt_status();
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::app.t);
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'STAGED', NULL::app.t);

-- Between commits, either way round.
SELECT length(dolt_commit('-m', 'second')::text) AS committed;
SELECT * FROM dolt_status();
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~1', 'HEAD', NULL::app.t);
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'HEAD~1', NULL::app.t);
SELECT count(*) FROM dolt_diff('HEAD', 'HEAD', NULL::app.t);

-- A keyless table's copies: each one added or removed is a row.
ALTER TABLE app.k DROP COLUMN b;
DELETE FROM app.k WHERE a = 'y';
INSERT INTO app.k VALUES ('z'), ('z');
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::app.k) ORDER BY 1, 2, 3;
SELECT * FROM dolt_status();

-- Another branch's head.
SELECT dolt_branch('f');
SELECT dolt_checkout('f');
UPDATE app.t SET v = 'on f' WHERE id = 1;
SELECT length(dolt_commit('-m', 'on f', '-a')::text) AS committed;
SELECT dolt_checkout('main');
SELECT diff_type, from_row, to_row FROM dolt_diff('main', 'f', NULL::app.t);

-- Bad refs and tables.
SELECT * FROM dolt_diff('nope', 'HEAD', NULL::app.t);
CREATE TABLE plain (x int);
SELECT * FROM dolt_diff('HEAD~1', 'HEAD', NULL::plain);
DROP TABLE plain;

SELECT dolt_branch('-D', 'f');
DROP SCHEMA app CASCADE;

-- A column added with a default: a row's NULL in it is NULL in the diff,
-- not the default (rows are folded, and read back, as a rewrite has them).
CREATE SCHEMA sdd VERSIONED;
CREATE TABLE sdd.t (id int PRIMARY KEY, a text);
INSERT INTO sdd.t VALUES (1, 'one'), (2, 'two');
SELECT length(dolt_commit('-Am', 'before the column')::text) AS committed;
ALTER TABLE sdd.t ADD COLUMN c int DEFAULT 5;
UPDATE sdd.t SET c = NULL WHERE id = 1;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::sdd.t)
  ORDER BY to_row;
DROP SCHEMA sdd CASCADE;
SELECT length(dolt_commit('-Am', 'without it')::text) AS committed;

-- Names in a database whose encoding isn't UTF-8 (the canonical tree's).
CREATE DATABASE sd_latin1 ENCODING 'LATIN1' LC_COLLATE 'C' LC_CTYPE 'C'
  TEMPLATE template0;
\c sd_latin1
SET client_encoding = 'UTF8';
CREATE SCHEMA "appé" VERSIONED;
CREATE TABLE "appé"."tablé" (id int PRIMARY KEY, v text);
INSERT INTO "appé"."tablé" VALUES (1, 'a');
SELECT length(dolt_commit('-Am', 'first')::text) AS committed;
INSERT INTO "appé"."tablé" VALUES (2, 'b');
SELECT * FROM dolt_status;
SELECT diff_type, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::"appé"."tablé");
\c contrib_regression
DROP DATABASE sd_latin1;

-- One whose encoding orders names otherwise than UTF-8 does ("я" before
-- "€" in UTF-8, after it in WIN1251): only the table dropped is.
CREATE DATABASE sd_win1251 ENCODING 'WIN1251' LC_COLLATE 'C' LC_CTYPE 'C'
  TEMPLATE template0;
\c sd_win1251
SET client_encoding = 'UTF8';
CREATE SCHEMA app VERSIONED;
CREATE TABLE app."я" (id int PRIMARY KEY);
CREATE TABLE app."€" (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'two tables')::text) AS committed;
DROP TABLE app."я";
SELECT * FROM dolt_status ORDER BY table_name;
\c contrib_regression
DROP DATABASE sd_win1251;
