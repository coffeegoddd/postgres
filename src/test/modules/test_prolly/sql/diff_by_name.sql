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

-- renamed (rows changed too, so matched as a drop and an add): by its old
-- name in the version that has it, and by its new
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
DROP OWNED BY regress_dn_reader;
DROP ROLE regress_dn_reader;

\c contrib_regression
DROP DATABASE regress_diff_by_name;
