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

-- a keyless table given a key since: its versions' rows aren't diffed
-- keyed by it (copies of a row would be one; a changed row, modified),
-- so the stat between them isn't counted through it
CREATE TABLE ds.nokey (id int, v text);
INSERT INTO ds.nokey VALUES (1, 'a'), (2, 'b');
SELECT length(dolt_commit('-Am', 'nokey')::text) AS committed;
UPDATE ds.nokey SET v = 'c' WHERE id = 2;
SELECT length(dolt_commit('-am', 'nokey changed')::text) AS committed;
SELECT table_name, rows_added, rows_deleted, rows_modified
FROM dolt_diff_stat('HEAD~1', 'HEAD', 'ds.nokey');
ALTER TABLE ds.nokey ADD PRIMARY KEY (id);
SELECT table_name, rows_added, rows_deleted, rows_modified
FROM dolt_diff_stat('HEAD~1', 'HEAD', 'ds.nokey');
DROP TABLE ds.nokey;

-- a table changed on both sides but gone from the branch: its rows
-- diffed from the commits' trees, each version decoded by its own
-- description; with a text key in a collation, too, whose order isn't its
-- bytes'; a keyless one with copies; and values out of line
CREATE TABLE ds.gone2 (id int PRIMARY KEY, v text);
INSERT INTO ds.gone2 SELECT g, 'v' || g FROM generate_series(1, 500) g;
CREATE TABLE ds.coll (k text COLLATE "en-x-icu" PRIMARY KEY, v int);
INSERT INTO ds.coll SELECT chr(65 + g % 26) || chr(97 + g % 26) || g, g FROM generate_series(1, 400) g;
CREATE TABLE ds.kl (a int, b text);
INSERT INTO ds.kl SELECT g % 7, repeat('x', 3000 * (g % 2)) || g % 7 FROM generate_series(1, 70) g;
CREATE TABLE ds.ool (id int PRIMARY KEY, v text);
INSERT INTO ds.ool SELECT g, repeat(md5(g::text), 400) FROM generate_series(1, 20) g;
SELECT length(dolt_commit('-Am', 'left')::text) AS committed;
UPDATE ds.gone2 SET v = 'changed' WHERE id % 50 = 0;
DELETE FROM ds.gone2 WHERE id > 490;
INSERT INTO ds.gone2 VALUES (1000, 'new'), (1001, 'new');
UPDATE ds.coll SET v = -v WHERE v % 40 = 0;
DELETE FROM ds.coll WHERE v BETWEEN 1 AND 5;
INSERT INTO ds.coll VALUES ('aa', 0), ('ZZ', 0), ('Ab', 0);
DELETE FROM ds.kl WHERE a = 3;
INSERT INTO ds.kl SELECT 9, 'new' FROM generate_series(1, 4);
UPDATE ds.ool SET v = repeat(md5((id + 1)::text), 400) WHERE id <= 3;
SELECT length(dolt_commit('-am', 'changed')::text) AS committed;
SELECT * FROM dolt_diff_stat('HEAD~1', 'HEAD', 'ds.gone2') AS here \gset here_
SELECT * FROM dolt_diff_stat('HEAD~1', 'HEAD', 'ds.coll') AS here \gset coll_
SELECT * FROM dolt_diff_stat('HEAD~1', 'HEAD', 'ds.kl') AS here \gset kl_
SELECT * FROM dolt_diff_stat('HEAD~1', 'HEAD', 'ds.ool') AS here \gset ool_
DROP TABLE ds.gone2, ds.coll, ds.kl, ds.ool;
SELECT length(dolt_commit('-am', 'dropped')::text) AS committed;
SELECT table_name, rows_unmodified, rows_added, rows_deleted, rows_modified,
	cells_added, cells_deleted, cells_modified, old_row_count, new_row_count
FROM dolt_diff_stat('HEAD~2', 'HEAD~1')
WHERE table_name IN ('ds.gone2', 'ds.coll', 'ds.kl', 'ds.ool')
ORDER BY 1;
SELECT rows_added = :here_rows_added AND rows_deleted = :here_rows_deleted AND
	rows_modified = :here_rows_modified AND cells_modified = :here_cells_modified
	AS gone2_as_on_the_branch
FROM dolt_diff_stat('HEAD~2', 'HEAD~1', 'ds.gone2');
SELECT rows_added = :coll_rows_added AND rows_deleted = :coll_rows_deleted AND
	rows_modified = :coll_rows_modified AND cells_modified = :coll_cells_modified
	AS coll_as_on_the_branch
FROM dolt_diff_stat('HEAD~2', 'HEAD~1', 'ds.coll');
SELECT rows_added = :kl_rows_added AND rows_deleted = :kl_rows_deleted
	AS kl_as_on_the_branch
FROM dolt_diff_stat('HEAD~2', 'HEAD~1', 'ds.kl');
SELECT rows_modified = :ool_rows_modified AND cells_modified = :ool_cells_modified
	AS ool_as_on_the_branch
FROM dolt_diff_stat('HEAD~2', 'HEAD~1', 'ds.ool');

-- a table the branch doesn't have, whose enum has had a label renamed
-- since: its old values don't decode as the type is now, so its rows are
-- counted and the cells modified are unknown
CREATE TYPE ds.mood AS ENUM ('old', 'kept');
CREATE TABLE ds.moody (id int PRIMARY KEY, m ds.mood);
INSERT INTO ds.moody VALUES (1, 'old'), (2, 'kept');
SELECT length(dolt_commit('-Am', 'moody')::text) AS committed;
UPDATE ds.moody SET m = 'kept' WHERE id = 1;
INSERT INTO ds.moody VALUES (3, 'kept');
SELECT length(dolt_commit('-am', 'moody changed')::text) AS committed;
DROP TABLE ds.moody;
ALTER TYPE ds.mood RENAME VALUE 'old' TO 'renamed';
SELECT table_name, rows_unmodified, rows_added, rows_deleted, rows_modified,
	cells_added, cells_deleted, cells_modified, old_row_count, new_row_count
FROM dolt_diff_stat('HEAD~1', 'HEAD', 'ds.moody');
DROP TYPE ds.mood;

\c contrib_regression
DROP DATABASE regress_diff_stat;
