-- Doltgres's Postgres-specific version-control scripts (testing/go at
-- doltgresql d5e05f46), run on prolly: merges of tables, constraints,
-- types and new schemas (merge_test.go, schemas_test.go); the objects
-- Doltgres calls root objects, through status, checkout, merge and reset
-- (root_object_collections_test.go), and a function both sides changed
-- (conflicts_root_object_test.go); information_schema per branch
-- (information_schema_test.go); sequences (sequences_test.go); and how the
-- functions are called (dolt_procedures_record_test.go).  Each script has
-- a versioned schema of its own, first on the search path.  A comment says
-- where prolly's answer differs, and why.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE FUNCTION pg_temp.committed(VARIADIC args text[]) RETURNS bool LANGUAGE sql AS $$
	SELECT length((dolt_commit(VARIADIC args))[1]) = 64
$$;

-- merge_test.go "simple merge"
CREATE SCHEMA dgo_simple VERSIONED;
SET search_path = dgo_simple;
CREATE TABLE t1 (a INT, b INT, c INT, PRIMARY KEY (a));
CREATE TABLE t2 (a INT, b INT, c INT, PRIMARY KEY (a));
INSERT INTO t1 VALUES (1, 2, 3), (4, 5, 6), (7, 8, 9);
INSERT INTO t2 VALUES (1, 2, 3), (4, 5, 6), (7, 8, 9);
SELECT pg_temp.committed('-Am', 'intial commit');
SELECT dolt_branch('dgo_branch1');
SELECT dolt_branch('dgo_branch2');
SELECT dolt_checkout('dgo_branch1');
INSERT INTO t1 VALUES (10, 11, 12);
INSERT INTO t2 VALUES (10, 11, 12);
SELECT pg_temp.committed('-Am', 'added 10');
SELECT dolt_checkout('dgo_branch2');
INSERT INTO t1 VALUES (20, 21, 22);
INSERT INTO t2 VALUES (20, 21, 22);
SELECT pg_temp.committed('-Am', 'added 20');
SELECT dolt_checkout('main');
SELECT (dolt_merge('dgo_branch1'))[2:4];
SELECT (dolt_merge('dgo_branch2'))[2:4];
SELECT * FROM t1 ORDER BY a;
SELECT * FROM t2 ORDER BY a;
SELECT dolt_branch('-D', 'dgo_branch1', 'dgo_branch2');
RESET search_path;

-- merge_test.go "merge independent JSON document changes": Doltgres
-- merges the two documents' keys; prolly compares a json value whole, so
-- the row conflicts (it merges column by column, not within a value)
CREATE SCHEMA dgo_json VERSIONED;
SET search_path = dgo_json;
CREATE TABLE documents_json (id INT PRIMARY KEY, doc JSON);
CREATE TABLE documents_jsonb (id INT PRIMARY KEY, doc JSONB);
INSERT INTO documents_json VALUES (1, '{"a": 1, "b": 2}');
INSERT INTO documents_jsonb VALUES (1, '{"a": 1, "b": 2}');
SELECT pg_temp.committed('-Am', 'base');
SELECT dolt_branch('dgo_left');
SELECT dolt_branch('dgo_right');
SELECT dolt_checkout('dgo_left');
UPDATE documents_json SET doc = '{"a": 100, "b": 2}' WHERE id = 1;
UPDATE documents_jsonb SET doc = doc || '{"a": 100}' WHERE id = 1;
SELECT pg_temp.committed('-am', 'left');
SELECT dolt_checkout('dgo_right');
UPDATE documents_json SET doc = '{"a": 1, "b": 200}' WHERE id = 1;
UPDATE documents_jsonb SET doc = doc || '{"b": 200}' WHERE id = 1;
SELECT pg_temp.committed('-am', 'right');
SELECT dolt_checkout('dgo_left');
BEGIN;
SELECT (dolt_merge('dgo_right'))[2:4];
SELECT * FROM dolt_conflicts WHERE table_name LIKE 'dgo_json.%' ORDER BY 1;
ROLLBACK;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'dgo_left', 'dgo_right');
RESET search_path;

-- merge_test.go "merge with check expressions and column defaults",
-- "merge with unique constraints and foreign keys"
CREATE SCHEMA dgo_cons VERSIONED;
SET search_path = dgo_cons;
SET timezone TO 'UTC';
CREATE TABLE t1 (a INT, b timestamptz default '2020-01-01 00:00:00'::timestamptz, PRIMARY KEY (a));
ALTER TABLE t1 ADD CONSTRAINT check_b CHECK (b >= '2020-01-01 00:00:00'::timestamptz);
INSERT INTO t1 VALUES (1, '2020-01-02 00:00:00'), (2, '2020-01-03 00:00:00');
CREATE TABLE u1 (a INT, b INT, PRIMARY KEY (a), unique (b));
CREATE TABLE u2 (a INT, b INT, PRIMARY KEY (a), foreign key (b) references u1(b));
INSERT INTO u1 VALUES (1, 2), (4, 5), (7, 8);
INSERT INTO u2 VALUES (1, 2), (4, 5), (7, 8);
SELECT pg_temp.committed('-Am', 'intial commit');
SELECT dolt_branch('dgo_branch1');
SELECT dolt_branch('dgo_branch2');
SELECT dolt_checkout('dgo_branch1');
INSERT INTO t1 VALUES (3, '2020-01-04 00:00:00');
INSERT INTO u1 VALUES (10, 11);
INSERT INTO u2 VALUES (10, 11);
SELECT pg_temp.committed('-Am', 'added 3, 10');
SELECT dolt_checkout('dgo_branch2');
INSERT INTO t1 VALUES (4, '2020-01-05 00:00:00');
INSERT INTO u1 VALUES (20, 21);
INSERT INTO u2 VALUES (20, 21);
SELECT pg_temp.committed('-Am', 'added 4, 20');
SELECT dolt_checkout('main');
SELECT (dolt_merge('dgo_branch1'))[2:4];
SELECT (dolt_merge('dgo_branch2'))[2:4];
SELECT * FROM t1 ORDER BY a;
INSERT INTO t1 VALUES (5, '2019-12-31 00:00:00');
SELECT * FROM u1 ORDER BY a;
INSERT INTO u1 VALUES (100, 2);
INSERT INTO u2 VALUES (100, 200);
RESET timezone;
SELECT dolt_branch('-D', 'dgo_branch1', 'dgo_branch2');
RESET search_path;

-- merge_test.go "merge a branch that created a type"
CREATE SCHEMA dgo_type VERSIONED;
SET search_path = dgo_type;
CREATE TABLE t1 (a INT PRIMARY KEY);
SELECT pg_temp.committed('-Am', 'initial commit');
SELECT dolt_checkout('-b', 'dgo_branch1');
CREATE TYPE type1 AS ENUM ('a', 'b');
CREATE TABLE t2 (a INT PRIMARY KEY, b type1);
INSERT INTO t2 VALUES (1, 'a');
SELECT pg_temp.committed('-Am', 'added type1');
SELECT dolt_checkout('main');
INSERT INTO t1 VALUES (1);
SELECT pg_temp.committed('-Am', 'added 1');
SELECT (dolt_merge('dgo_branch1'))[2:4];
SELECT * FROM t2;
SELECT 'b'::type1;
SELECT dolt_branch('-D', 'dgo_branch1');
RESET search_path;

-- schemas_test.go "add new table in new schema, commit, status", "... commit
-- -Am", "create new schema with no tables, add and commit": a schema is
-- versioned when made so (CREATE SCHEMA ... VERSIONED); dolt_add stages
-- nothing (every change is committed), so status shows it staged at once
CREATE SCHEMA dgo_myschema VERSIONED;
CREATE TABLE dgo_myschema.mytbl (pk BIGINT PRIMARY KEY, v1 BIGINT);
SELECT * FROM dolt_status WHERE table_name LIKE 'dgo_myschema%' ORDER BY 1;
SELECT dolt_add('.');
SELECT pg_temp.committed('-m', 'new table in new schema');
SELECT * FROM dolt_status WHERE table_name LIKE 'dgo_myschema%';
SELECT message FROM dolt_log ORDER BY commit_order DESC LIMIT 1;
CREATE SCHEMA dgo_empty VERSIONED;
SELECT * FROM dolt_status WHERE table_name LIKE 'dgo_empty%';
SELECT pg_temp.committed('-Am', 'new schema');
SELECT * FROM dolt_status WHERE table_name LIKE 'dgo_empty%';
SELECT message FROM dolt_log ORDER BY commit_order DESC LIMIT 1;

-- schemas_test.go "merge new table in new schema"
SELECT dolt_checkout('-b', 'dgo_schemas');
CREATE SCHEMA dgo_branchschema VERSIONED;
CREATE TABLE dgo_branchschema.mytbl (pk BIGINT PRIMARY KEY, v1 BIGINT);
INSERT INTO dgo_branchschema.mytbl VALUES (1, 1), (2, 2);
CREATE TABLE dgo_branchschema.mytbl2 (pk BIGINT PRIMARY KEY, v1 BIGINT);
INSERT INTO dgo_branchschema.mytbl2 VALUES (3, 3), (4, 4);
SELECT pg_temp.committed('-Am', 'new table in new schema');
SELECT dolt_checkout('main');
CREATE SCHEMA dgo_mainschema VERSIONED;
CREATE TABLE dgo_mainschema.maintable (pk BIGINT PRIMARY KEY, v1 BIGINT);
INSERT INTO dgo_mainschema.maintable VALUES (5, 5), (6, 6);
SELECT pg_temp.committed('-Am', 'new table in main');
SELECT (dolt_merge('dgo_schemas'))[2:4];
SELECT * FROM dgo_mainschema.maintable ORDER BY pk;
SELECT * FROM dgo_branchschema.mytbl ORDER BY pk;
SELECT * FROM dgo_branchschema.mytbl2 ORDER BY pk;
SELECT dolt_branch('-D', 'dgo_schemas');

-- root_object_collections_test.go "dropping every root object restores
-- the committed root": each object made shows in status, and dropping it
-- leaves none (a domain is one object, where Doltgres lists its array type
-- too; a trigger and a cast are their table's or type's definition)
CREATE SCHEMA dgo_roots VERSIONED;
SET search_path = dgo_roots;
CREATE TABLE t1 (pk INTEGER PRIMARY KEY, v1 INTEGER);
CREATE TABLE cast_src (v TEXT);
CREATE TABLE cast_dst (v TEXT);
SELECT pg_temp.committed('-Am', 'initial');
SELECT * FROM dolt_status WHERE table_name LIKE 'dgo_roots.%';
CREATE SEQUENCE s1;
SELECT * FROM dolt_status WHERE table_name LIKE 'dgo_roots.%';
DROP SEQUENCE s1;
SELECT * FROM dolt_status WHERE table_name LIKE 'dgo_roots.%';
CREATE DOMAIN d1 AS INTEGER;
SELECT * FROM dolt_status WHERE table_name LIKE 'dgo_roots.%';
DROP DOMAIN d1;
SELECT * FROM dolt_status WHERE table_name LIKE 'dgo_roots.%';
CREATE FUNCTION f1() RETURNS INTEGER AS $$ BEGIN RETURN 1; END; $$ LANGUAGE plpgsql;
SELECT * FROM dolt_status WHERE table_name LIKE 'dgo_roots.%';
DROP FUNCTION f1;
SELECT * FROM dolt_status WHERE table_name LIKE 'dgo_roots.%';
CREATE PROCEDURE p1() AS $$ BEGIN END; $$ LANGUAGE plpgsql;
SELECT * FROM dolt_status WHERE table_name LIKE 'dgo_roots.%';
DROP PROCEDURE p1;
SELECT * FROM dolt_status WHERE table_name LIKE 'dgo_roots.%';
CREATE FUNCTION tf1() RETURNS trigger AS $$ BEGIN RETURN NEW; END; $$ LANGUAGE plpgsql;
CREATE TRIGGER tr1 BEFORE INSERT ON t1 FOR EACH ROW EXECUTE FUNCTION tf1();
DROP TRIGGER tr1 ON t1;
DROP FUNCTION tf1;
SELECT * FROM dolt_status WHERE table_name LIKE 'dgo_roots.%';
CREATE FUNCTION cf1(cast_src) RETURNS cast_dst AS $$ SELECT ROW(($1).v)::cast_dst $$ LANGUAGE SQL;
CREATE CAST (cast_src AS cast_dst) WITH FUNCTION cf1(cast_src);
DROP CAST (cast_src AS cast_dst);
DROP FUNCTION cf1;
SELECT * FROM dolt_status WHERE table_name LIKE 'dgo_roots.%';

-- "root objects follow checkout, merge, and reset": a sequence and a
-- function made on a branch are its own until merged; a hard reset drops
-- an uncommitted sequence.  Sequences are versioned here (Doltgres's are
-- global): the reset puts the merged s1 back as committed, unused, so its
-- next value is 1 again, where Doltgres's is 2.
SELECT dolt_checkout('-b', 'dgo_other');
CREATE SEQUENCE s1;
CREATE FUNCTION f1() RETURNS INTEGER AS $$ BEGIN RETURN 1; END; $$ LANGUAGE plpgsql;
SELECT f1();
SELECT dolt_add('.');
SELECT pg_temp.committed('-m', 'other');
SELECT dolt_checkout('main');
SELECT f1();
SELECT nextval('s1');
SELECT (dolt_merge('dgo_other'))[2:4];
SELECT f1();
SELECT nextval('s1');
CREATE SEQUENCE s2;
SELECT dolt_reset('--hard');
SELECT nextval('s2');
SELECT f1();
SELECT nextval('s1');
SELECT dolt_branch('-D', 'dgo_other');
RESET search_path;

-- conflicts_root_object_test.go: a function both sides changed
-- differently.  Doltgres records a conflict to resolve by choosing a
-- side's definition; prolly's merge of definitions fails on it, a schema
-- conflict.  Changed on one side only, it merges.
CREATE SCHEMA dgo_fn VERSIONED;
SET search_path = dgo_fn;
CREATE FUNCTION interpreted_example(input TEXT) RETURNS TEXT AS $$ BEGIN RETURN '1' || input; END; $$ LANGUAGE plpgsql;
CREATE TABLE t (a int PRIMARY KEY);
SELECT pg_temp.committed('-Am', 'initial');
SELECT dolt_branch('dgo_fn_other');
CREATE OR REPLACE FUNCTION interpreted_example(input TEXT) RETURNS TEXT AS $$ BEGIN RETURN '2' || input; END; $$ LANGUAGE plpgsql;
SELECT pg_temp.committed('-Am', 'main');
SELECT dolt_checkout('dgo_fn_other');
CREATE OR REPLACE FUNCTION interpreted_example(input TEXT) RETURNS TEXT AS $$ BEGIN RETURN '3' || input; END; $$ LANGUAGE plpgsql;
SELECT pg_temp.committed('-Am', 'other');
SELECT dolt_checkout('main');
SELECT dolt_merge('dgo_fn_other');
SELECT interpreted_example('x');
SELECT dolt_checkout('-b', 'dgo_fn_mine');
CREATE OR REPLACE FUNCTION interpreted_example(input TEXT) RETURNS TEXT AS $$ BEGIN RETURN '4' || input; END; $$ LANGUAGE plpgsql;
SELECT pg_temp.committed('-Am', 'one side');
SELECT dolt_checkout('main');
INSERT INTO dgo_fn.t VALUES (1);
SELECT pg_temp.committed('-Am', 'main moves on');
SELECT (dolt_merge('dgo_fn_mine'))[2:4];
SELECT interpreted_example('x');
SELECT dolt_branch('-D', 'dgo_fn_other', 'dgo_fn_mine');
RESET search_path;

-- information_schema_test.go "info_schema changes with dolt_checkout"
CREATE SCHEMA dgo_info VERSIONED;
SET search_path = dgo_info;
CREATE TABLE t (a int primary key, b int);
SELECT pg_temp.committed('-Am', 'creating table t');
SELECT dolt_branch('dgo_b2');
SELECT dolt_branch('dgo_b3');
SELECT dolt_checkout('dgo_b2');
ALTER TABLE t ADD COLUMN c int;
SELECT pg_temp.committed('-am', 'added column c on branch b2');
SELECT dolt_checkout('dgo_b3');
ALTER TABLE t ADD COLUMN d int;
SELECT pg_temp.committed('-am', 'added column d on branch b3');
SELECT dolt_checkout('main');
SELECT active_branch();
SELECT column_name FROM information_schema.columns WHERE table_schema = 'dgo_info' AND table_name = 't' ORDER BY 1;
SELECT dolt_checkout('dgo_b2');
SELECT active_branch();
SELECT column_name FROM information_schema.columns WHERE table_schema = 'dgo_info' AND table_name = 't' ORDER BY 1;
SELECT dolt_checkout('dgo_b3');
SELECT active_branch();
SELECT column_name FROM information_schema.columns WHERE table_schema = 'dgo_info' AND table_name = 't' ORDER BY 1;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'dgo_b2', 'dgo_b3');
RESET search_path;

-- sequences_test.go "sequences are globally tracked ...": Doltgres's
-- sequences are global, one value across branches and resets; prolly's
-- are versioned, each branch's as committed or changed there (the other
-- branch has the value committed when it was made; a hard reset puts the
-- committed value back).  "Error when branches contain incompatible
-- sequence definitions": here each branch has its own definition.
CREATE SCHEMA dgo_seq VERSIONED;
SET search_path = dgo_seq;
SELECT pg_temp.committed('-Am', 'dgo_seq made');
CREATE SEQUENCE test;
SELECT setval('test', 10);
SELECT nextval('test');
SELECT dolt_add('test');
SELECT pg_temp.committed('-m', 'initial');
SELECT dolt_branch('dgo_seq_other');
SELECT setval('test', 20);
SELECT dolt_add('.');
SELECT pg_temp.committed('-m', 'next');
SELECT nextval('test');
SELECT dolt_checkout('dgo_seq_other');
SELECT nextval('test');
SELECT dolt_reset('--hard');
SELECT nextval('test');
CREATE SEQUENCE test2;
SELECT dolt_checkout('-b', 'dgo_seq_incompatible');
CREATE SEQUENCE test2 INCREMENT -1;
SELECT nextval('test2');
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'dgo_seq_other', 'dgo_seq_incompatible');
RESET search_path;

-- dolt_procedures_record_test.go: Doltgres's functions return a record,
-- which a FROM clause explodes into columns (status, message; hash);
-- prolly's return a text[] (D3), one column, whose elements are read by
-- subscript
SELECT * FROM dolt_checkout('-b', 'dgo_newbranch');
SELECT r[2] AS message FROM dolt_checkout('main') r;
SELECT * FROM dolt_checkout('dgo_newbranch') AS t(a);
SELECT * FROM dolt_add('.');
CREATE TABLE dgo_info.t1 (pk int primary key);
SELECT count(*) FROM dolt_commit('-Am', 'new table') h WHERE length(h[1]) = 64;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'dgo_newbranch');

-- cursor_test.go "cursors over table functions and system tables": a
-- cursor over dolt_log, dolt_diff and dolt_status declared, then a commit,
-- then the fetches.  A set-returning function's cursor runs the function
-- at its first fetch, as Postgres's do, so these see the commit made
-- after the declarations (Doltgres's see the state at declaration):
-- dolt_log's two newest are third and second, and the working set's diff
-- and status are empty.  (dolt_diff_stat, dolt_patch and dolt_diff_<t>
-- aren't prolly's.)
CREATE SCHEMA dgo_cur VERSIONED;
SET search_path = dgo_cur;
CREATE TABLE tf (id INT4 PRIMARY KEY, v TEXT);
INSERT INTO tf VALUES (1, 'a');
SELECT pg_temp.committed('-Am', 'first');
INSERT INTO tf VALUES (2, 'b');
SELECT pg_temp.committed('-am', 'second');
UPDATE tf SET v = 'c' WHERE id = 1;
BEGIN;
DECLARE lg CURSOR FOR SELECT message FROM dolt_log() ORDER BY commit_order DESC LIMIT 2;
DECLARE df CURSOR FOR SELECT (to_row).id, (to_row).v, (from_row).id, (from_row).v, diff_type FROM dolt_diff('HEAD~1', 'HEAD', NULL::tf);
DECLARE dw CURSOR FOR SELECT (to_row).id, (to_row).v, (from_row).v, diff_type FROM dolt_diff('HEAD', 'WORKING', NULL::tf);
DECLARE st CURSOR FOR SELECT table_name, staged, status FROM dolt_status WHERE table_name LIKE 'dgo_cur.%';
DECLARE gs SCROLL CURSOR FOR SELECT * FROM generate_series(1, 5);
UPDATE tf SET v = 'z' WHERE id = 2;
SELECT pg_temp.committed('-am', 'third');
FETCH ALL FROM lg;
FETCH ALL FROM df;
FETCH ALL FROM dw;
FETCH ALL FROM st;
FETCH 3 FROM gs;
FETCH BACKWARD 2 FROM gs;
ROLLBACK;
RESET search_path;

SET client_min_messages = warning;
DROP SCHEMA dgo_cur, dgo_simple, dgo_json, dgo_cons, dgo_type, dgo_myschema, dgo_empty,
	dgo_branchschema, dgo_mainschema, dgo_roots, dgo_fn, dgo_info, dgo_seq CASCADE;
RESET client_min_messages;
SELECT pg_temp.committed('-Am', 'doltgres objects done');
