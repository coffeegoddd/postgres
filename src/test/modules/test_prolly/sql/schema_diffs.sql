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

-- rows only, definition only, both; added, dropped, renamed
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
RESET search_path;

-- a rename that changed the rows too is a drop and an add
ALTER TABLE app.new_name RENAME TO newer_name;
UPDATE app.newer_name SET v = 'changed';
CREATE TABLE app.twin_a (id int PRIMARY KEY);
CREATE TABLE app.twin_b (id int PRIMARY KEY);
CREATE TABLE app.same_a (v int);
CREATE TABLE app.same_b (v int);
SELECT length(dolt_commit('-Am', 'twins')::text) AS committed;
SELECT * FROM dolt_diff_summary('HEAD~1', 'HEAD');
-- two renamed at once, told apart by what else names them (their keys'
-- indexes keep their names); two of one content renamed can't be told
-- apart, so they stay dropped and added
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
SELECT * FROM dolt_diff_summary('HEAD', 'WORKING') WHERE diff_type = 'not covered';
DROP FOREIGN TABLE app.ft;

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

-- a commit of an older format: refused, as moves refuse it
SELECT test_commit_of(test_root_with_format(prolly_root_hash('HEAD'), '7'),
	'format 7') AS old \gset
SELECT * FROM dolt_schema_diff(:'old', 'HEAD');
SELECT count(*) > 0 AS summarized FROM dolt_diff_summary(:'old', 'HEAD');
SELECT * FROM dolt_schema_diff(NULL, 'HEAD');

\c contrib_regression
DROP DATABASE regress_schema_diffs;
