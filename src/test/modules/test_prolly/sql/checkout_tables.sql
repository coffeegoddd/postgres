-- dolt_checkout of tables: their uncommitted changes discarded (put back
-- as the head, or a commit, has them), the rest of the working set left
-- as it is.
CREATE DATABASE regress_checkout_tables;
\c regress_checkout_tables
CREATE SCHEMA ct VERSIONED;
CREATE TYPE ct.mood AS ENUM ('sad', 'ok');
CREATE TABLE ct.a (id int PRIMARY KEY, v text);
CREATE TABLE ct.b (id int PRIMARY KEY, v text);
CREATE TABLE ct.m (id int PRIMARY KEY, m ct.mood);
INSERT INTO ct.a VALUES (1, 'a');
INSERT INTO ct.b VALUES (1, 'b');
INSERT INTO ct.m VALUES (1, 'ok');
SELECT length(dolt_commit('-Am', 'three tables')::text) AS committed;

-- one of two changed tables discarded, the other kept
UPDATE ct.a SET v = 'a2';
UPDATE ct.b SET v = 'b2';
SELECT dolt_checkout('--', 'ct.a');
SELECT * FROM ct.a;
SELECT * FROM ct.b;
SELECT table_name, status FROM dolt_status ORDER BY 1;

-- a name that is no branch but a table, without --; schema-qualified or
-- by the search path
SET search_path = ct, public;
SELECT dolt_checkout('b');
SELECT * FROM ct.b;
SELECT count(*) AS changed FROM dolt_status;
RESET search_path;

-- a dropped table put back; a new one taken away; a column added undone
DROP TABLE ct.a;
CREATE TABLE ct.n (id int);
ALTER TABLE ct.b ADD COLUMN w int;
SELECT dolt_checkout('--', 'ct.a', 'ct.n', 'ct.b');
SELECT * FROM ct.a;
SELECT to_regclass('ct.n') IS NULL AS new_gone;
SELECT count(*) AS b_columns FROM information_schema.columns
WHERE table_schema = 'ct' AND table_name = 'b';
SELECT count(*) AS changed FROM dolt_status;

-- "." : every table the head has; one only the working set has stays,
-- as Dolt's untracked table does
UPDATE ct.a SET v = 'x';
UPDATE ct.b SET v = 'y';
CREATE TABLE ct.untracked (id int PRIMARY KEY);
INSERT INTO ct.untracked VALUES (9);
SELECT dolt_checkout('.');
SELECT table_name, status FROM dolt_status ORDER BY 1;
SELECT * FROM ct.untracked;
DROP TABLE ct.untracked;
SELECT count(*) AS changed FROM dolt_status;

-- as another commit has them: ref, then tables; or ref -- tables, the
-- ref before "--" taken as one even where a table is named like it
UPDATE ct.a SET v = 'later';
SELECT length(dolt_commit('-am', 'later')::text) AS committed;
SELECT dolt_checkout('HEAD~1', 'ct.a');
SELECT * FROM ct.a;
SELECT table_name, status FROM dolt_status;
SELECT dolt_reset('--hard');
SELECT dolt_checkout('HEAD~1', '--', 'ct.a');
SELECT * FROM ct.a;
SELECT dolt_reset('--hard');
SELECT dolt_checkout('HEAD~1', 'HEAD', '--', 'ct.a');

-- a table whose sequence moved: the sequence with it, as the head has it
CREATE TABLE ct.s (id serial PRIMARY KEY, v text);
INSERT INTO ct.s (v) VALUES ('one');
SELECT length(dolt_commit('-Am', 'serial')::text) AS committed;
INSERT INTO ct.s (v) VALUES ('two'), ('three');
SELECT dolt_checkout('--', 'ct.s');
SELECT * FROM ct.s;
INSERT INTO ct.s (v) VALUES ('next');
SELECT * FROM ct.s ORDER BY id;
SELECT dolt_reset('--hard');

-- a table needing a type both versions hold otherwise: refused
ALTER TYPE ct.mood ADD VALUE 'happy';
UPDATE ct.m SET m = 'happy';
SELECT dolt_checkout('--', 'ct.m');
SELECT dolt_reset('--hard');

-- a name that is neither; one that is a branch too goes to the branch
SELECT dolt_checkout('--', 'ct.nope');
SELECT dolt_checkout('-b', 'ct.a');
SELECT dolt_checkout('main');
SELECT dolt_checkout('ct.a');
SELECT active_branch();
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'ct.a');
-- a branch before "--", alone, is the branch, though a table has its name
SET search_path = ct, public;
SELECT dolt_branch('b');
UPDATE ct.b SET v = 'dirty';
SELECT dolt_checkout('b', '--');
SELECT active_branch();
SELECT dolt_checkout('main');
SELECT * FROM ct.b;
SELECT dolt_reset('--hard');
SELECT dolt_branch('-D', 'b');
RESET search_path;
-- a table before "--" that names no ref is a table, with any after it, as
-- the head has them
UPDATE ct.a SET v = 'dirty';
SELECT dolt_checkout('ct.a', '--');
SELECT table_name, status FROM dolt_status;
UPDATE ct.a SET v = 'dirty';
UPDATE ct.b SET v = 'dirty';
SELECT dolt_checkout('ct.a', '--', 'ct.b');
SELECT table_name, status FROM dolt_status;

-- the sequences a table draws from come with it, found by its defaults:
-- not one a text literal names, nor one whose name begins another's; one
-- quoted, and one two tables share, do
CREATE SEQUENCE ct.unrelated;
CREATE SEQUENCE ct.seq;
CREATE SEQUENCE ct.seq2;
CREATE SEQUENCE ct."Quoted Seq";
CREATE SEQUENCE ct.shared;
CREATE TABLE ct.co (id int PRIMARY KEY, label text DEFAULT 'ct.unrelated',
	n int DEFAULT nextval('ct.seq2'), q int DEFAULT nextval('ct."Quoted Seq"'),
	sh int DEFAULT nextval('ct.shared'));
CREATE TABLE ct.co2 (id int PRIMARY KEY, sh int DEFAULT nextval('ct.shared'));
SELECT length(dolt_commit('-Am', 'sequences')::text) AS committed;
INSERT INTO ct.co (id) VALUES (1);
SELECT nextval('ct.unrelated') AS unrelated, nextval('ct.seq') AS seq;
SELECT dolt_checkout('--', 'ct.co');
SELECT count(*) AS co_rows FROM ct.co;
SELECT 'unrelated' AS seq, last_value, is_called FROM ct.unrelated
UNION ALL SELECT 'seq', last_value, is_called FROM ct.seq
UNION ALL SELECT 'seq2', last_value, is_called FROM ct.seq2
UNION ALL SELECT 'Quoted Seq', last_value, is_called FROM ct."Quoted Seq"
UNION ALL SELECT 'shared', last_value, is_called FROM ct.shared;
SELECT dolt_reset('--hard');

-- on a protected branch, a checkout that would leave it changed is
-- refused, as a write is; one that changes nothing isn't
INSERT INTO ct.co2 (id) VALUES (1);
SELECT length(dolt_commit('-am', 'co2')::text) AS committed;
SELECT dolt_branch('--protect', 'main');
SELECT dolt_checkout('HEAD~1', 'ct.co2');
SELECT count(*) AS changed FROM dolt_status;
SELECT dolt_checkout('--', 'ct.co2');
SELECT dolt_branch('--unprotect', 'main');

\c contrib_regression
DROP DATABASE regress_checkout_tables;
