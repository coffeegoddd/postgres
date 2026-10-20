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

-- "." : every table
UPDATE ct.a SET v = 'x';
UPDATE ct.b SET v = 'y';
SELECT dolt_checkout('.');
SELECT count(*) AS changed FROM dolt_status;

-- as another commit has them: ref, then tables
UPDATE ct.a SET v = 'later';
SELECT length(dolt_commit('-am', 'later')::text) AS committed;
SELECT dolt_checkout('HEAD~1', 'ct.a');
SELECT * FROM ct.a;
SELECT table_name, status FROM dolt_status;
SELECT dolt_reset('--hard');

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

\c contrib_regression
DROP DATABASE regress_checkout_tables;
