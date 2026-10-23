-- dolt_diff_summary: each object that differs between two refs, tables
-- and the rest, named as dolt_status names them: added, dropped,
-- modified or renamed, with whether its rows and its definition differ.
CREATE DATABASE regress_schema_diffs;
\c regress_schema_diffs
CREATE EXTENSION test_prolly;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
INSERT INTO app.t VALUES (1, 'a'), (2, 'b');
CREATE TABLE app.keep (id int PRIMARY KEY);
INSERT INTO app.keep VALUES (1);
CREATE TABLE app.old_name (id int PRIMARY KEY, v text);
INSERT INTO app.old_name VALUES (1, 'moved');
CREATE TABLE app.gone (id int PRIMARY KEY);
CREATE VIEW app.vw AS SELECT id FROM app.t;
CREATE FUNCTION app.f() RETURNS int LANGUAGE sql AS 'SELECT 1';
CREATE SEQUENCE app.seq;
CREATE TYPE app.mood AS ENUM ('sad', 'ok');
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;

-- nothing changed: nothing
SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');

-- rows only, definition only, both; added, dropped, renamed (an empty
-- table added or dropped changes no rows, and a new name is a change of
-- definition, as in Dolt)
UPDATE app.t SET v = 'z' WHERE id = 1;
ALTER TABLE app.keep ADD COLUMN note text;
ALTER TABLE app.old_name RENAME TO new_name;
DROP TABLE app.gone;
CREATE TABLE app.fresh (id int PRIMARY KEY);
CREATE OR REPLACE VIEW app.vw AS SELECT id, v FROM app.t;
CREATE OR REPLACE FUNCTION app.f() RETURNS int LANGUAGE sql AS 'SELECT 2';
SELECT nextval('app.seq');
ALTER TYPE app.mood ADD VALUE 'happy';
CREATE FUNCTION app.g(int) RETURNS int LANGUAGE sql AS 'SELECT $1';
SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');

-- between commits the same; and the other way round
SELECT length(dolt_commit('-Am', 'changes')::text) AS committed;
SELECT * FROM dolt_diff_summary('HEAD~1', 'HEAD');
SELECT from_table_name, to_table_name, diff_type FROM dolt_diff_summary('HEAD', 'HEAD~1');

-- one object, by its name qualified or on the search path, either version's
SELECT * FROM dolt_diff_summary('HEAD~1', 'HEAD', 'app.t');
SET search_path = app, public;
SELECT * FROM dolt_diff_summary('HEAD~1', 'HEAD', 'f()');
SELECT * FROM dolt_diff_summary('HEAD~1', 'HEAD', 'gone');
SELECT * FROM dolt_diff_summary('HEAD~1', 'HEAD', 'old_name');
SELECT * FROM dolt_diff_summary('HEAD~1', 'HEAD', 'nothing_by_this_name');
-- names read as identifiers: case folded, quotes kept
SELECT from_table_name FROM dolt_diff_summary('HEAD~1', 'HEAD', 'APP.T');
SELECT from_table_name FROM dolt_diff_summary('HEAD~1', 'HEAD', 'T');
SELECT from_table_name FROM dolt_diff_summary('HEAD~1', 'HEAD', '"T"');
SELECT from_table_name FROM dolt_diff_summary('HEAD~1', 'HEAD', 'F()');
RESET search_path;

-- a rename that changed the rows too is one renamed table, by its
-- identity, which the rename keeps (as Dolt's tags)
ALTER TABLE app.new_name RENAME TO newer_name;
UPDATE app.newer_name SET v = 'changed';
CREATE TABLE app.twin_a (id int PRIMARY KEY);
CREATE TABLE app.twin_b (id int PRIMARY KEY);
CREATE TABLE app.same_a (v int);
CREATE TABLE app.same_b (v int);
SELECT length(dolt_commit('-Am', 'twins')::text) AS committed;
SELECT * FROM dolt_diff_summary('HEAD~1', 'HEAD');
-- two renamed at once, each told by its identity, even two of one content
ALTER TABLE app.twin_a RENAME TO twin_c;
ALTER TABLE app.twin_b RENAME TO twin_d;
ALTER TABLE app.same_a RENAME TO same_c;
ALTER TABLE app.same_b RENAME TO same_d;
SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');
-- a view renamed is one too
ALTER VIEW app.vw RENAME TO vw2;
SELECT * FROM dolt_diff_summary('HEAD', 'WORKING', 'app.vw2');

-- what a commit can't hold, in the working set: not covered, flags unknown
CREATE FOREIGN DATA WRAPPER sdiff_fdw;
CREATE SERVER sdiff_srv FOREIGN DATA WRAPPER sdiff_fdw;
CREATE FOREIGN TABLE app.ft (id int) SERVER sdiff_srv;
CREATE FUNCTION public.sdiff_noop() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END';
CREATE TRIGGER noop BEFORE INSERT ON app.ft FOR EACH ROW EXECUTE FUNCTION public.sdiff_noop();
SELECT * FROM dolt_diff_summary('HEAD', 'WORKING') WHERE diff_type = 'not covered';
DROP FOREIGN TABLE app.ft;
DROP FUNCTION public.sdiff_noop();

-- refs that aren't
SELECT * FROM dolt_diff_summary('HEAD', 'no_such_ref');
SELECT * FROM dolt_diff_summary(NULL, 'HEAD');

-- dolt_schema_diff: each object whose statements differ, each side's
-- rendered from that version's description, never the catalogs.
CREATE TABLE app.orders (id int PRIMARY KEY, qty int NOT NULL DEFAULT 1,
	note text COLLATE "C", CONSTRAINT qty_positive CHECK (qty > 0));
CREATE INDEX orders_qty ON app.orders (qty);
CREATE TABLE app.lines (id int PRIMARY KEY, order_id int REFERENCES app.orders);
ALTER TABLE app.orders ENABLE ROW LEVEL SECURITY;
CREATE POLICY own_orders ON app.orders USING (qty < 100);
SELECT length(dolt_commit('-Am', 'orders')::text) AS committed;
SELECT * FROM dolt_schema_diff('HEAD~1', 'HEAD');

-- changed, renamed, dropped; a change of rows alone gives none
ALTER TABLE app.orders ADD COLUMN placed date;
DROP INDEX app.orders_qty;
INSERT INTO app.orders VALUES (1, 2, 'x', NULL);
ALTER TABLE app.lines RENAME TO order_lines;
CREATE OR REPLACE FUNCTION app.f() RETURNS int LANGUAGE sql AS 'SELECT 3';
DROP TYPE app.mood;
UPDATE app.t SET v = 'only rows' WHERE id = 2;
SELECT * FROM dolt_schema_diff('HEAD', 'WORKING');
SELECT from_table_name, to_table_name FROM dolt_schema_diff('HEAD', 'WORKING', 'app.t');

-- views, sequences, types, schemas made
CREATE SCHEMA more VERSIONED;
CREATE SEQUENCE more.counter START 10 INCREMENT 5;
CREATE TYPE more.pair AS (a int, b text);
CREATE VIEW more.big AS SELECT id FROM app.orders WHERE qty > 10;
SELECT * FROM dolt_schema_diff('HEAD', 'WORKING') WHERE to_table_name LIKE 'more%';

-- ranges, as Dolt takes them: a..b is a to b; a...b from their merge base
-- to b; the argument after a range names the object
SELECT length(dolt_commit('-Am', 'before the fork')::text) AS committed;
SELECT dolt_branch('fork');
CREATE TABLE app.on_main (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'on main')::text) AS committed;
SELECT dolt_checkout('fork');
CREATE TABLE app.on_fork (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'on fork')::text) AS committed;
SELECT dolt_checkout('main');
SELECT from_table_name, to_table_name, diff_type FROM dolt_diff_summary('main..fork');
SELECT from_table_name, to_table_name, diff_type FROM dolt_diff_summary('main', 'fork');
SELECT from_table_name, to_table_name, diff_type FROM dolt_diff_summary('main...fork');
SELECT from_table_name, to_table_name FROM dolt_diff_summary('main...fork', 'app.on_fork');
SELECT to_table_name FROM dolt_schema_diff('main...fork');
SELECT table_name, rows_added FROM dolt_diff_stat('fork...main');
SELECT * FROM dolt_diff_summary('main..');
SELECT * FROM dolt_diff_summary('main..fork', 'app.on_fork', 'app.on_main');
SELECT * FROM dolt_diff_summary('main');
SELECT * FROM dolt_diff_summary('main...nowhere');

-- a commit of an older format: refused, as moves refuse it
SELECT test_commit_of(test_root_with_format(prolly_root_hash('HEAD'), '7'),
	'format 7') AS old \gset
SELECT * FROM dolt_schema_diff(:'old', 'HEAD');
SELECT count(*) > 0 AS summarized FROM dolt_diff_summary(:'old', 'HEAD');
SELECT * FROM dolt_schema_diff(NULL, 'HEAD');

-- a table dropped and made again under its name, with other rows, is
-- modified (Dolt's "table drop and recreate with identical primary key");
-- its key's index keeps the name it was made with, so its definition
-- differs too
CREATE TABLE app.t1 (id int PRIMARY KEY, name varchar(10));
INSERT INTO app.t1 VALUES (1, 'dolt');
SELECT length(dolt_commit('-Am', 'create_table')::text) AS committed;
UPDATE app.t1 SET name = 'doltgres' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'update_value')::text) AS committed;
CREATE TABLE app.temp__t1 (id int PRIMARY KEY, name varchar(10));
INSERT INTO app.temp__t1 VALUES (1, 'dolt');
DROP TABLE app.t1;
ALTER TABLE app.temp__t1 RENAME TO t1;
SELECT length(dolt_commit('-Am', 'drop and recreate')::text) AS committed;
SELECT * FROM dolt_diff_summary('HEAD~1', 'HEAD', 'app.t1');

-- a parenthesis inside a quoted identifier isn't a routine's arguments
CREATE SCHEMA "s(x" VERSIONED;
CREATE TABLE "s(x"."a(b" (id int PRIMARY KEY);
INSERT INTO "s(x"."a(b" VALUES (1);
CREATE FUNCTION "s(x"."f(n"(int) RETURNS int LANGUAGE sql AS 'SELECT $1';
SELECT to_table_name, diff_type
FROM dolt_diff_summary('HEAD', 'WORKING', '"s(x"."a(b"');
SELECT to_table_name, diff_type
FROM dolt_diff_summary('HEAD', 'WORKING', '"s(x"."f(n"(integer)');
SET search_path = "s(x", public;
SELECT to_table_name, diff_type
FROM dolt_diff_summary('HEAD', 'WORKING', '"a(b"');
RESET search_path;
SELECT table_name, rows_added
FROM dolt_diff_stat('HEAD', 'WORKING', '"s(x"."a(b"');

\c contrib_regression
DROP DATABASE regress_schema_diffs;

-- Doltgres's diff function scripts (testing/go/dolt_functions_test.go at
-- doltgresql d5e05f46: "smoke test select dolt diff functions and tables",
-- the same "for multiple schemas", and the summary and schema diff of
-- "dolt table functions WITH ORDINALITY"), their dolt_diff_summary and
-- dolt_schema_diff assertions.  Where prolly's answer differs from
-- Doltgres's: statements are Postgres's DDL, rendered from the commit's
-- description (a table's NOT NULLs and key as ALTER TABLE statements);
-- other kinds of objects than tables are listed too (a versioned schema
-- made); a name not qualified is found on the search path, where Doltgres
-- finds it in any schema; a database starts with no commits (Doltgres's
-- with its own), so one is made first.
CREATE DATABASE regress_doltgres_diffs;
\c regress_doltgres_diffs
DROP SCHEMA public;
CREATE SCHEMA public VERSIONED;
SELECT length(dolt_commit('--allow-empty', '-m', 'initial')::text) AS committed;

-- smoke test select dolt diff functions and tables
CREATE TABLE t1 (pk int primary key);
INSERT INTO t1 VALUES (1);
SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');
SELECT * FROM dolt_diff_summary('HEAD', 'WORKING', 't1');
SELECT * FROM dolt_schema_diff('HEAD', 'WORKING');
SELECT * FROM dolt_schema_diff('HEAD', 'WORKING', 't1');

-- ... for multiple schemas
SELECT dolt_reset('--hard');
CREATE TABLE t1 (pk int primary key);
INSERT INTO t1 VALUES (1);
CREATE SCHEMA testschema VERSIONED;
CREATE TABLE testschema.t2 (pk int primary key);
INSERT INTO testschema.t2 VALUES (1);
SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');
SELECT * FROM dolt_diff_summary('HEAD', 'WORKING', 't1');
SELECT * FROM dolt_diff_summary('HEAD', 'WORKING', 't2');
SET search_path = public, testschema;
SELECT * FROM dolt_diff_summary('HEAD', 'WORKING', 't2');
RESET search_path;
SELECT * FROM dolt_schema_diff('HEAD', 'WORKING');
SELECT * FROM dolt_schema_diff('HEAD', 'WORKING', 't1');
SELECT * FROM dolt_schema_diff('HEAD', 'WORKING', 'testschema.t2');

-- WITH ORDINALITY (skipped in Doltgres, where these are table functions
-- of its engine; Postgres's set-returning functions take it)
SELECT dolt_reset('--hard');
CREATE TABLE t1 (pk INT4 PRIMARY KEY, v1 TEXT);
SELECT length(dolt_commit('-A', '-m', 'initial')::text) AS committed;
INSERT INTO t1 VALUES (1, 'one'), (2, 'two');
SELECT to_table_name, diff_type, ordinality
	FROM dolt_diff_summary('HEAD', 'WORKING') WITH ORDINALITY;
SELECT * FROM dolt_schema_diff('HEAD', 'WORKING') WITH ORDINALITY;

\c contrib_regression
DROP DATABASE regress_doltgres_diffs;
