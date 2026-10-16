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
