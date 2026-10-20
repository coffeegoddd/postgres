-- dolt_diff_stat: for each table whose rows differ between two refs, as
-- Dolt counts them: rows added, deleted, modified and unmodified, the cells
-- of the modified rows that differ, and each side's rows and cells.
CREATE DATABASE regress_diff_stat;
\c regress_diff_stat
CREATE EXTENSION test_prolly;
CREATE SCHEMA ds VERSIONED;
CREATE TABLE ds.t (id int PRIMARY KEY, a int, b text);
INSERT INTO ds.t SELECT g, g, 'b' || g FROM generate_series(1, 10) g;
CREATE TABLE ds.bag (v int);
INSERT INTO ds.bag VALUES (1), (1), (2);
CREATE TABLE ds.big (id int PRIMARY KEY, body text STORAGE EXTERNAL);
INSERT INTO ds.big VALUES (1, repeat('x', 100000)), (2, repeat('y', 100000));
CREATE TABLE ds.gone (id int PRIMARY KEY);
INSERT INTO ds.gone VALUES (1), (2);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;

-- nothing changed
SELECT * FROM dolt_diff_stat('HEAD', 'WORKING');

-- two rows added, one deleted, two modified (one in both columns)
INSERT INTO ds.t VALUES (11, 11, 'b11'), (12, 12, 'b12');
DELETE FROM ds.t WHERE id = 1;
UPDATE ds.t SET a = 20 WHERE id = 2;
UPDATE ds.t SET a = 30, b = 'changed' WHERE id = 3;
-- a keyless table: copies added and taken away
INSERT INTO ds.bag VALUES (1), (3);
DELETE FROM ds.bag WHERE v = 2;
-- a value stored out of line changed
UPDATE ds.big SET body = repeat('z', 100000) WHERE id = 2;
-- a table added, one dropped
CREATE TABLE ds.fresh (id int PRIMARY KEY, x int);
INSERT INTO ds.fresh VALUES (1, 1), (2, 2), (3, 3);
DROP TABLE ds.gone;
SELECT * FROM dolt_diff_stat('HEAD', 'WORKING');

-- between commits the same; one table by name
SELECT length(dolt_commit('-Am', 'changes')::text) AS committed;
SELECT * FROM dolt_diff_stat('HEAD~1', 'HEAD');
SELECT table_name, rows_added, rows_deleted, rows_modified
FROM dolt_diff_stat('HEAD~1', 'HEAD', 'ds.t');
SELECT table_name, rows_deleted FROM dolt_diff_stat('HEAD~1', 'HEAD', 'ds.gone');

-- a column added changes every row's cells, but no row
ALTER TABLE ds.fresh ADD COLUMN y int;
SELECT * FROM dolt_diff_stat('HEAD', 'WORKING');
-- a column renamed changes nothing it counts
ALTER TABLE ds.fresh DROP COLUMN y;
ALTER TABLE ds.t RENAME COLUMN a TO aa;
SELECT * FROM dolt_diff_stat('HEAD', 'WORKING');

SELECT * FROM dolt_diff_stat(NULL, 'HEAD');

-- a table's item begins with its rows' tree hash, which can hold a zero
-- byte (this one's does): the item is read whole, not as a string
SELECT length(dolt_commit('-Am', 'renamed')::text) AS committed;
CREATE TABLE ds.zero (id int, c1 varchar(20));
SELECT length(dolt_commit('-Am', 'zero')::text) AS committed;
INSERT INTO ds.zero VALUES (1, 'one');
SELECT table_name, rows_added, rows_deleted
FROM dolt_diff_stat('HEAD', 'WORKING', 'ds.zero');
SELECT to_create_statement FROM dolt_schema_diff('HEAD~', 'WORKING', 'ds.zero');

-- a name neither version has a table of: refused, as Dolt's stat refuses
-- it; a table both have unchanged, none; the summary gives none either way
SELECT * FROM dolt_diff_stat('HEAD~1', 'HEAD', 'doesnotexist');
SELECT * FROM dolt_diff_stat('HEAD~1..HEAD', 'ds.nope');
SELECT count(*) FROM dolt_diff_stat('HEAD', 'WORKING', 'ds.t');
SELECT count(*) FROM dolt_diff_summary('HEAD~1', 'HEAD', 'doesnotexist');
-- a name whose changes are a view's, not a table's: refused too
CREATE VIEW ds.justview AS SELECT 1 AS one;
SELECT * FROM dolt_diff_stat('HEAD', 'WORKING', 'ds.justview');
SELECT to_table_name, diff_type
FROM dolt_diff_summary('HEAD', 'WORKING', 'ds.justview');
DROP VIEW ds.justview;

\c contrib_regression
DROP DATABASE regress_diff_stat;
