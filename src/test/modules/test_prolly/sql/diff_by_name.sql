-- A table's rows diffed by its name (dolt_diff, dolt_diff_table and
-- dolt_history_table given the table's name as text): so a table the
-- session hasn't, dropped since or renamed, can be read, each version's
-- rows as jsonb, decoded as that version defines the table (#612).
CREATE DATABASE regress_diff_by_name;
\c regress_diff_by_name
CREATE EXTENSION test_prolly;
CREATE SCHEMA dn VERSIONED;
SET search_path = dn, public;
CREATE TABLE dn.t1 (a int PRIMARY KEY, b int);
INSERT INTO dn.t1 VALUES (1, 2), (3, 4);
SELECT length(dolt_commit('-Am', 'new table')::text) AS committed;

-- a table that exists now: the same rows as its row type gives, as jsonb
UPDATE dn.t1 SET b = 20 WHERE a = 1;
INSERT INTO dn.t1 VALUES (5, 6);
DELETE FROM dn.t1 WHERE a = 3;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', 'dn.t1')
ORDER BY coalesce(to_row, from_row)->>'a';
SELECT count(*) AS same_rows FROM (
	SELECT diff_type, to_jsonb(from_row) AS f, to_jsonb(to_row) AS t
	FROM dolt_diff('HEAD', 'WORKING', NULL::dn.t1)
	INTERSECT
	SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', 't1')) s;
SELECT length(dolt_commit('-am', 'changed')::text) AS committed;

-- a column added: each version's rows with its own columns
ALTER TABLE dn.t1 ADD COLUMN c text;
UPDATE dn.t1 SET c = 'x' WHERE a = 5;
SELECT length(dolt_commit('-am', 'c added')::text) AS committed;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~', 'HEAD', 't1')
ORDER BY coalesce(to_row, from_row)->>'a';

-- dropped: its rows removed, between the commits before and after the drop
DROP TABLE dn.t1;
SELECT length(dolt_commit('-am', 'dropped table')::text) AS committed;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~', 'HEAD', 'dn.t1')
ORDER BY from_row->>'a';
SELECT diff_type, to_row FROM dolt_diff('HEAD', 'HEAD~', 'dn.t1')
ORDER BY to_row->>'a';
-- (its history by name, the row-type forms having no type to take)
SELECT diff_type, from_row FROM dolt_diff_table('dn.t1')
WHERE to_commit = dolt_hashof('HEAD') ORDER BY from_row->>'a';
SELECT count(*) AS changes FROM dolt_diff_table('dn.t1');
SELECT c.message, h.row FROM dolt_history_table('dn.t1') h
JOIN dolt_log c ON c.commit_hash = h.commit_hash
ORDER BY c.date, h.row->>'a';

-- renamed, rows changed too (one table, by its identity, as Dolt's tags
-- match it): by its old name or its new, the row added alone
CREATE TABLE dn.r1 (pk int PRIMARY KEY, v text);
INSERT INTO dn.r1 VALUES (1, 'one');
SELECT length(dolt_commit('-Am', 'r1')::text) AS committed;
ALTER TABLE dn.r1 RENAME TO r2;
INSERT INTO dn.r2 VALUES (2, 'two');
SELECT length(dolt_commit('-am', 'renamed')::text) AS committed;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~', 'HEAD', 'r1');
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~', 'HEAD', 'r2')
ORDER BY to_row->>'pk';
-- renamed alone: the same rows, none differ
ALTER TABLE dn.r2 RENAME TO r3;
SELECT length(dolt_commit('-am', 'renamed again')::text) AS committed;
SELECT count(*) FROM dolt_diff('HEAD~', 'HEAD', 'r2');
SELECT count(*) FROM dolt_diff('HEAD~', 'HEAD', 'r3');

-- a keyless table's copies, dropped: each copy a row
CREATE TABLE dn.bag (i int);
INSERT INTO dn.bag VALUES (1), (1), (2);
SELECT length(dolt_commit('-Am', 'bag')::text) AS committed;
DROP TABLE dn.bag;
SELECT length(dolt_commit('-am', 'bag dropped')::text) AS committed;
SELECT diff_type, from_row FROM dolt_diff('HEAD~', 'HEAD', 'bag')
ORDER BY from_row->>'i';

-- a name neither version has is refused; the row-type form, unchanged
SELECT * FROM dolt_diff('HEAD~', 'HEAD', 'no_such_table');
SELECT * FROM dolt_diff('HEAD~', 'HEAD', NULL::int);
SELECT * FROM dolt_diff('HEAD~', 'HEAD', NULL::text);

-- a version whose column's type this database hasn't now is refused
CREATE TYPE dn.mood AS ENUM ('ok', 'sad');
CREATE TABLE dn.feel (id int PRIMARY KEY, m dn.mood);
INSERT INTO dn.feel VALUES (1, 'ok');
SELECT length(dolt_commit('-Am', 'feel')::text) AS committed;
DROP TABLE dn.feel;
DROP TYPE dn.mood;
SELECT length(dolt_commit('-am', 'feel dropped')::text) AS committed;
SELECT * FROM dolt_diff('HEAD~', 'HEAD', 'feel');

-- the table of a version's name the session has: SELECT on it; a dropped
-- one, no grant to check: the database's owner, or pg_read_all_data
CREATE ROLE regress_dn_reader;
GRANT USAGE ON SCHEMA dn TO regress_dn_reader;
SET ROLE regress_dn_reader;
SELECT count(*) FROM dolt_diff('HEAD~5', 'HEAD~4', 'r3');
SELECT count(*) FROM dolt_diff('HEAD~3', 'HEAD~2', 'bag');
RESET ROLE;
GRANT SELECT ON dn.r3 TO regress_dn_reader;
SET ROLE regress_dn_reader;
SELECT count(*) FROM dolt_diff('HEAD~5', 'HEAD~4', 'r3');
SELECT count(*) FROM dolt_diff('HEAD~3', 'HEAD~2', 'bag');
RESET ROLE;
GRANT pg_read_all_data TO regress_dn_reader;
SET ROLE regress_dn_reader;
SELECT count(*) FROM dolt_diff('HEAD~3', 'HEAD~2', 'bag');
RESET ROLE;
REVOKE pg_read_all_data FROM regress_dn_reader;

-- a table made with a dropped one's name isn't it: SELECT on it doesn't
-- cover the dropped one's rows, by any of the three, only its own
CREATE TABLE dn.secret (id int PRIMARY KEY, s text);
INSERT INTO dn.secret VALUES (1, 'top secret');
SELECT length(dolt_commit('-Am', 'secret')::text) AS committed;
DROP TABLE dn.secret;
SELECT length(dolt_commit('-am', 'secret dropped')::text) AS committed;
CREATE TABLE dn.secret (id int PRIMARY KEY, note text);
INSERT INTO dn.secret VALUES (2, 'public');
GRANT SELECT ON dn.secret TO regress_dn_reader;
SELECT length(dolt_commit('-Am', 'public secret')::text) AS committed;
SET ROLE regress_dn_reader;
SELECT * FROM dolt_diff('HEAD~2', 'HEAD~', 'dn.secret');
SELECT * FROM dolt_diff('HEAD~2', 'HEAD', 'dn.secret');
SELECT diff_type, from_row FROM dolt_diff_table('dn.secret');
SELECT row FROM dolt_history_table('dn.secret');
SELECT diff_type, to_row FROM dolt_diff('HEAD~', 'HEAD', 'dn.secret');
SELECT diff_type, to_row FROM dolt_diff_table('dn.secret', 'HEAD~');
RESET ROLE;
-- (the database's owner reads both)
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~2', 'HEAD', 'dn.secret');

-- nor one made with a renamed one's old name: the renamed one's rows by
-- that name, without SELECT on it
CREATE TABLE dn.k1 (id int PRIMARY KEY, s text);
INSERT INTO dn.k1 VALUES (1, 'kept');
SELECT length(dolt_commit('-Am', 'k1')::text) AS committed;
ALTER TABLE dn.k1 RENAME TO k2;
SELECT length(dolt_commit('-am', 'k1 renamed')::text) AS committed;
CREATE TABLE dn.k1 (id int PRIMARY KEY);
GRANT SELECT ON dn.k1 TO regress_dn_reader;
SELECT length(dolt_commit('-Am', 'k1 again')::text) AS committed;
SET ROLE regress_dn_reader;
SELECT * FROM dolt_diff('HEAD~3', 'HEAD~2', 'k1');
SELECT count(*) FROM dolt_history_table('k1');
RESET ROLE;
GRANT SELECT ON dn.k2 TO regress_dn_reader;
SET ROLE regress_dn_reader;
-- (k2's grant covers its version by the name it had, across the rename)
SELECT count(*) FROM dolt_diff('HEAD~2', 'HEAD~', 'k2');
SELECT count(*) FROM dolt_diff_table('k2');
RESET ROLE;

-- a diff of two commits by name beside uncommitted changes: the
-- database's owner reads no working-set row (no lineage to walk); a
-- reader with a grant gets the same rows
INSERT INTO dn.k2 VALUES (2, 'two');
SELECT length(dolt_commit('-am', 'k2 two')::text) AS committed;
INSERT INTO dn.k2 SELECT g, 'dirty' FROM generate_series(10, 509) g;
SELECT count(*) AS reset FROM test_fold_counters(true);
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~', 'HEAD', 'k2');
SELECT pages, candidates, full_tables FROM test_fold_counters(true);
SET ROLE regress_dn_reader;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~', 'HEAD', 'k2');
RESET ROLE;
DELETE FROM dn.k2 WHERE id >= 10;
DROP OWNED BY regress_dn_reader;
DROP ROLE regress_dn_reader;

\c contrib_regression
DROP DATABASE regress_diff_by_name;
