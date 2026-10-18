-- dolt_status and dolt_diff of branches made by copying: versions compared
-- as their folded trees.

CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
CREATE TABLE app.gone (x int);
CREATE TABLE app.k (a text);
INSERT INTO app.t SELECT g, 'v' || g FROM generate_series(1, 2000) g;
INSERT INTO app.k VALUES ('x'), ('y'), ('y');
INSERT INTO app.gone VALUES (1);
SELECT length(dolt_commit('-m', 'first')::text) AS committed;
SELECT * FROM dolt_status();

-- Rows changed, a table made, one dropped, one redefined.
UPDATE app.t SET v = 'changed' WHERE id IN (7, 1500);
DELETE FROM app.t WHERE id = 3;
INSERT INTO app.t VALUES (2001, 'new');
CREATE TABLE app.made (y int);
DROP TABLE app.gone;
ALTER TABLE app.k ADD COLUMN b int;
SELECT * FROM dolt_status();
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::app.t);
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'STAGED', NULL::app.t);

-- Between commits, either way round.
SELECT length(dolt_commit('-m', 'second')::text) AS committed;
SELECT * FROM dolt_status();
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~1', 'HEAD', NULL::app.t);
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'HEAD~1', NULL::app.t);
SELECT count(*) FROM dolt_diff('HEAD', 'HEAD', NULL::app.t);

-- A keyless table's copies: each one added or removed is a row.
ALTER TABLE app.k DROP COLUMN b;
DELETE FROM app.k WHERE a = 'y';
INSERT INTO app.k VALUES ('z'), ('z');
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::app.k) ORDER BY 1, 2, 3;
SELECT * FROM dolt_status();

-- Another branch's head.
SELECT dolt_branch('f');
SELECT dolt_checkout('f');
UPDATE app.t SET v = 'on f' WHERE id = 1;
SELECT length(dolt_commit('-m', 'on f', '-a')::text) AS committed;
SELECT dolt_checkout('main');
SELECT diff_type, from_row, to_row FROM dolt_diff('main', 'f', NULL::app.t);

-- Bad refs and tables.
SELECT * FROM dolt_diff('nope', 'HEAD', NULL::app.t);
CREATE TABLE plain (x int);
SELECT * FROM dolt_diff('HEAD~1', 'HEAD', NULL::plain);
DROP TABLE plain;

SELECT dolt_branch('-D', 'f');
DROP SCHEMA app CASCADE;

-- A column added with a default: a row's NULL in it is NULL in the diff,
-- not the default (rows are folded, and read back, as a rewrite has them).
CREATE SCHEMA sdd VERSIONED;
CREATE TABLE sdd.t (id int PRIMARY KEY, a text);
INSERT INTO sdd.t VALUES (1, 'one'), (2, 'two');
SELECT length(dolt_commit('-Am', 'before the column')::text) AS committed;
ALTER TABLE sdd.t ADD COLUMN c int DEFAULT 5;
UPDATE sdd.t SET c = NULL WHERE id = 1;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::sdd.t)
  ORDER BY to_row;
DROP SCHEMA sdd CASCADE;
SELECT length(dolt_commit('-Am', 'without it')::text) AS committed;

-- Names in a database whose encoding isn't UTF-8 (the canonical tree's).
CREATE DATABASE sd_latin1 ENCODING 'LATIN1' LC_COLLATE 'C' LC_CTYPE 'C'
  TEMPLATE template0;
\c sd_latin1
SET client_encoding = 'UTF8';
CREATE SCHEMA "appé" VERSIONED;
CREATE TABLE "appé"."tablé" (id int PRIMARY KEY, v text);
INSERT INTO "appé"."tablé" VALUES (1, 'a');
SELECT length(dolt_commit('-Am', 'first')::text) AS committed;
INSERT INTO "appé"."tablé" VALUES (2, 'b');
SELECT * FROM dolt_status;
SELECT diff_type, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::"appé"."tablé");
\c contrib_regression
DROP DATABASE sd_latin1;

-- One whose encoding orders names otherwise than UTF-8 does ("я" before
-- "€" in UTF-8, after it in WIN1251): only the table dropped is.
CREATE DATABASE sd_win1251 ENCODING 'WIN1251' LC_COLLATE 'C' LC_CTYPE 'C'
  TEMPLATE template0;
\c sd_win1251
SET client_encoding = 'UTF8';
CREATE SCHEMA app VERSIONED;
CREATE TABLE app."я" (id int PRIMARY KEY);
CREATE TABLE app."€" (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'two tables')::text) AS committed;
DROP TABLE app."я";
SELECT * FROM dolt_status ORDER BY table_name;
\c contrib_regression
DROP DATABASE sd_win1251;

-- Reading what changed doesn't need what a commit does: objects the
-- canonical form doesn't describe (a foreign table) are left out of status
-- and diff, and only a commit refuses them.
CREATE FOREIGN DATA WRAPPER sd_fdw;
CREATE SERVER sd_srv FOREIGN DATA WRAPPER sd_fdw;
CREATE SCHEMA sdu VERSIONED;
CREATE TABLE sdu.t (id int PRIMARY KEY, v text);
SELECT length(dolt_commit('-Am', 'sdu')::text) AS committed;
-- They are changes a commit can't hold: listed as not covered, and the
-- branch is dirty with them alone.
CREATE FOREIGN TABLE sdu.ft (id int) SERVER sd_srv;
SELECT name, dirty FROM dolt_branches WHERE name = 'main';
SELECT * FROM dolt_status WHERE table_name LIKE 'sdu.%';
INSERT INTO sdu.t VALUES (1, 'one');
SELECT * FROM dolt_status WHERE table_name LIKE 'sdu.%';
SELECT diff_type, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::sdu.t);
SELECT name, dirty FROM dolt_branches WHERE name = 'main';
SELECT dolt_commit('-am', 'refused');
DROP SCHEMA sdu CASCADE;
SELECT length(dolt_commit('-Am', 'sdu gone')::text) AS committed;

-- A column whose values are local OIDs (oid, a large object's reference
-- through a domain over it, the reg* types, and types made of them) is
-- one too: status lists it and still shows its table's rows; a commit
-- refuses it, rather than versioning the numbers.
CREATE SCHEMA sdl VERSIONED;
CREATE DOMAIN sdl.lo AS oid;
CREATE TYPE sdl.pair AS (a int, b regclass);
CREATE TABLE sdl.t (id int PRIMARY KEY, v text);
SELECT length(dolt_commit('-Am', 'sdl')::text) AS committed;
ALTER TABLE sdl.t ADD COLUMN doc sdl.lo;
INSERT INTO sdl.t VALUES (1, 'one', 16400);
SELECT * FROM dolt_status WHERE table_name LIKE 'sdl.%' ORDER BY table_name;
SELECT diff_type, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::sdl.t);
SELECT dolt_commit('-am', 'a large object');
-- only a commit refuses it: a reset discards the column, as any
-- uncommitted change
SELECT dolt_reset('--hard');
SELECT count(*) AS doc_columns FROM pg_attribute
  WHERE attrelid = 'sdl.t'::regclass AND attname = 'doc';
CREATE TABLE sdl.u (id int PRIMARY KEY, o oid, c regclass, r oid[], p sdl.pair,
	x oid8);
SELECT dolt_commit('-Am', 'oids');
SELECT * FROM dolt_status WHERE table_name LIKE 'sdl.%' ORDER BY table_name;
DROP TABLE sdl.u;
-- domains over arrays, ranges and multiranges (of an ordinary schema's
-- range type), arrays of composites, and pg_node_tree, whose text names
-- objects by OID
CREATE SCHEMA sdl_shared;
CREATE TYPE sdl_shared.oidrange AS RANGE (subtype = oid);
CREATE DOMAIN sdl.los AS oid[];
CREATE TABLE sdl.w (id int PRIMARY KEY, a sdl.los, g sdl_shared.oidrange,
	m sdl_shared.oidmultirange, ps sdl.pair[], n pg_node_tree, ok int[]);
SELECT dolt_commit('-Am', 'more oids');
SELECT * FROM dolt_status WHERE table_name LIKE 'sdl.w%' ORDER BY table_name;
DROP TABLE sdl.w;
CREATE MATERIALIZED VIEW sdl.mv USING prolly AS SELECT 'sdl.t'::regclass AS c;
SELECT dolt_commit('-Am', 'a materialized view');
DROP MATERIALIZED VIEW sdl.mv;
SELECT length(dolt_commit('-Am', 'no oids')::text) AS committed;
DROP SCHEMA sdl CASCADE;
DROP SCHEMA sdl_shared CASCADE;
SELECT length(dolt_commit('-Am', 'sdl gone')::text) AS committed;

-- A diff of one table reads that table's changes alone, however many
-- others changed.
CREATE SCHEMA sdo VERSIONED;
CREATE TABLE sdo.one (id int PRIMARY KEY, v text);
CREATE TABLE sdo.other (id int PRIMARY KEY, v text);
INSERT INTO sdo.one VALUES (1, 'a');
INSERT INTO sdo.other SELECT g, 'x' FROM generate_series(1, 5000) g;
SELECT length(dolt_commit('-Am', 'sdo')::text) AS committed;
UPDATE sdo.one SET v = 'b';
UPDATE sdo.other SET v = 'y';
SELECT count(*) AS reset FROM test_fold_counters(true);
SELECT diff_type, to_row FROM dolt_diff('HEAD', 'WORKING', NULL::sdo.one);
SELECT pages < 10 AS only_its_pages, full_tables FROM test_fold_counters(true);
SELECT count(*) FROM dolt_diff('WORKING', 'WORKING', NULL::sdo.one);
SELECT pages < 10 AS only_its_pages FROM test_fold_counters(true);
DROP SCHEMA sdo CASCADE;
SELECT length(dolt_commit('-Am', 'sdo gone')::text) AS committed;

-- dolt_diff takes a table's row type; a large value, stored out of line
-- (uncompressed: STORAGE EXTERNAL), diffs whole.
SELECT * FROM dolt_diff('HEAD', 'WORKING', NULL::int);
CREATE SCHEMA sdt VERSIONED;
CREATE TABLE sdt.t (id int PRIMARY KEY, big text);
ALTER TABLE sdt.t ALTER COLUMN big SET STORAGE EXTERNAL;
INSERT INTO sdt.t VALUES (1, repeat('a', 6000));
SELECT pg_column_toast_chunk_id(big) IS NOT NULL AS out_of_line FROM sdt.t;
SELECT length(dolt_commit('-Am', 'sdt')::text) AS committed;
UPDATE sdt.t SET big = repeat('b', 7000);
SELECT pg_column_toast_chunk_id(big) IS NOT NULL AS out_of_line FROM sdt.t;
SELECT diff_type, length((from_row).big), left((from_row).big, 1),
	length((to_row).big), left((to_row).big, 1)
  FROM dolt_diff('HEAD', 'WORKING', NULL::sdt.t);

-- a foreign table makes a commit fail
CREATE FOREIGN TABLE sdt.ft (id int) SERVER sd_srv;
SELECT dolt_commit('-Am', 'foreign table');
DROP FOREIGN TABLE sdt.ft;
DROP SCHEMA sdt CASCADE;
SELECT length(dolt_commit('-Am', 'sdt gone')::text) AS committed;

-- dolt_status lists objects other than tables that changed, by qualified
-- name, as Doltgres does: views, functions (with their argument types),
-- sequences (their definitions and their values) and schemas.
SELECT * FROM dolt_status;
CREATE SCHEMA sdv VERSIONED;
CREATE TABLE sdv.t (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'sdv')::text) AS committed;
SELECT * FROM dolt_status;
CREATE VIEW sdv.v AS SELECT id FROM sdv.t;
CREATE FUNCTION sdv.f(int) RETURNS int LANGUAGE sql AS 'SELECT 1';
CREATE SEQUENCE sdv.s;
SELECT * FROM dolt_status ORDER BY table_name;
SELECT length(dolt_commit('-Am', 'sdv objects')::text) AS committed;
SELECT * FROM dolt_status;
CREATE OR REPLACE VIEW sdv.v AS SELECT id, 1 AS one FROM sdv.t;
DROP FUNCTION sdv.f(int);
SELECT nextval('sdv.s');
SELECT * FROM dolt_status ORDER BY table_name;
DROP SCHEMA sdv CASCADE;
SELECT * FROM dolt_status ORDER BY table_name;
SELECT length(dolt_commit('-Am', 'sdv gone')::text) AS committed;

-- A head whose sequences have no values recorded (an older build's, or a
-- fold that couldn't read them) isn't a change of their values.
CREATE SCHEMA sdq VERSIONED;
CREATE SEQUENCE sdq.s;
SELECT nextval('sdq.s');
SELECT length(dolt_commit('-Am', 'sdq')::text) AS committed;
SELECT dolt_hashof('HEAD') AS sdq_head \gset
SELECT test_commit_of(test_root_without_sequence_values(test_fold_root()),
					  'no values') AS novalues \gset
SELECT test_refs_set_head(:'novalues');
SELECT * FROM dolt_status;
SELECT test_refs_set_head(:'sdq_head');

-- A function recreated as an aggregate is listed once, as modified (not
-- also as deleted).
CREATE FUNCTION sdq.f(int) RETURNS int LANGUAGE sql AS 'SELECT 1';
SELECT length(dolt_commit('-Am', 'sdq f')::text) AS committed;
DROP FUNCTION sdq.f(int);
CREATE AGGREGATE sdq.f(int) (SFUNC = int4pl, STYPE = int);
SELECT * FROM dolt_status ORDER BY table_name;
DROP SCHEMA sdq CASCADE;
SELECT length(dolt_commit('-Am', 'sdq gone')::text) AS committed;
DROP SERVER sd_srv;
DROP FOREIGN DATA WRAPPER sd_fdw;
