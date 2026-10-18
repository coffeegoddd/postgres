-- The branches a transaction writes: rows of its versioned tables and
-- sequences, and its versioned objects' catalog rows; not reads, not
-- unversioned objects, and not what is written on the session's behalf
-- (a branch's copies made, a branch moved to a commit).  A subtransaction
-- that aborts forgets what it first wrote.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE SCHEMA bw VERSIONED;
CREATE TABLE bw.t (id int PRIMARY KEY, v text);
CREATE SEQUENCE bw.s;
CREATE TABLE bw_plain (id int PRIMARY KEY, v text);
CREATE SEQUENCE bw_plain_s;
INSERT INTO bw.t SELECT g, 'v' || g FROM generate_series(1, 20) g;
SELECT length(dolt_commit('-m', 'bw base')::text) AS committed;

-- Nothing yet; reads and unversioned writes are no branch's.
BEGIN;
SELECT test_branches_written() AS written;
SELECT count(*) FROM bw.t;
SELECT * FROM bw.t WHERE id = 1 FOR UPDATE;
INSERT INTO bw_plain VALUES (1, 'a');
SELECT nextval('bw_plain_s');
SELECT test_branches_written() AS written;
COMMIT;

-- Rows: insert, update, delete, and COPY.
BEGIN;
INSERT INTO bw.t VALUES (100, 'x');
SELECT test_branches_written() AS written;
ROLLBACK;
BEGIN;
UPDATE bw.t SET v = 'y' WHERE id = 2;
SELECT test_branches_written() AS written;
ROLLBACK;
BEGIN;
DELETE FROM bw.t WHERE id = 3;
SELECT test_branches_written() AS written;
ROLLBACK;
BEGIN;
COPY bw.t FROM stdin;
200	copied
\.
SELECT test_branches_written() AS written;
ROLLBACK;

-- A versioned sequence's value.
BEGIN;
SELECT nextval('bw.s');
SELECT test_branches_written() AS written;
ROLLBACK;
BEGIN;
SELECT setval('bw.s', 10);
SELECT test_branches_written() AS written;
ROLLBACK;

-- Definitions: made, changed and dropped; TRUNCATE.
BEGIN;
CREATE TABLE bw.u (id int);
SELECT test_branches_written() AS written;
ROLLBACK;
BEGIN;
COMMENT ON TABLE bw.t IS 'changed';
SELECT test_branches_written() AS written;
ROLLBACK;
BEGIN;
DROP SEQUENCE bw.s;
SELECT test_branches_written() AS written;
ROLLBACK;
BEGIN;
TRUNCATE bw.t;
SELECT test_branches_written() AS written;
ROLLBACK;
-- (an unversioned definition is no branch's)
BEGIN;
CREATE TABLE bw_plain2 (id int);
DROP TABLE bw_plain2;
SELECT test_branches_written() AS written;
ROLLBACK;

-- A subtransaction that aborts forgets what it first wrote; one that
-- commits keeps it; and what was written before it stays.
BEGIN;
SAVEPOINT a;
INSERT INTO bw.t VALUES (101, 'x');
ROLLBACK TO a;
SELECT test_branches_written() AS written;
SAVEPOINT b;
INSERT INTO bw.t VALUES (102, 'x');
RELEASE b;
SELECT test_branches_written() AS written;
SAVEPOINT c;
INSERT INTO bw.t VALUES (103, 'x');
ROLLBACK TO c;
SELECT test_branches_written() AS written;
ROLLBACK;
BEGIN;
DO $$
BEGIN
	INSERT INTO bw.t VALUES (104, 'x');
	RAISE EXCEPTION 'undo';
EXCEPTION WHEN raise_exception THEN
	NULL;
END $$;
SELECT test_branches_written() AS written;
ROLLBACK;

-- On the session's behalf: making a branch, checking one out (its copies
-- made at first use), and deleting one, write nothing of the transaction's.
BEGIN;
SELECT dolt_branch('bw_f');
SELECT dolt_checkout('bw_f');
SELECT test_branches_written() AS written;
INSERT INTO bw.t VALUES (105, 'on f');
SELECT test_branches_written() AS written;
SELECT dolt_checkout('main');
INSERT INTO bw.t VALUES (106, 'on main');
SELECT test_branches_written() AS written;
ROLLBACK;
SELECT dolt_branch('bw_g');
BEGIN;
SELECT dolt_branch('-d', 'bw_g');
SELECT test_branches_written() AS written;
ROLLBACK;

-- What changes the session's branch as a write would is the branch's write
-- (said once, before), though its own writes are made on its behalf.
INSERT INTO bw.t VALUES (107, 'dirty');
BEGIN;
SELECT dolt_reset('--hard');
SELECT test_branches_written() AS written;
COMMIT;
SELECT count(*) FROM bw.t WHERE id = 107;
BEGIN;
SELECT dolt_commit('-m', 'nothing', '--allow-empty') IS NOT NULL AS committed;
SELECT test_branches_written() AS written;
ROLLBACK;

-- Nothing lasts past the transaction.
BEGIN;
INSERT INTO bw.t VALUES (108, 'x');
COMMIT;
BEGIN;
SELECT test_branches_written() AS written;
ROLLBACK;
SELECT dolt_reset('--hard');
SELECT dolt_branch('-d', 'bw_g');
DROP TABLE bw_plain;
DROP SEQUENCE bw_plain_s;
