-- An extension can be installed in a versioned schema, and isn't versioned
-- by it: what its script makes there is shared, every branch's, with one
-- OID, as an extension in an ordinary schema is.  The schema's own objects
-- stay the branch's.  A table the script makes there is a shared heap table.
-- While an extension is in it, the schema can't be dropped or renamed on any
-- branch, and objects can't move between the extension and the schema's
-- versioned objects.  (In a database of its own, so the branches here are
-- the only ones.)

CREATE DATABASE regress_versioned_extensions;
\c regress_versioned_extensions

CREATE SCHEMA ve VERSIONED;
CREATE TABLE ve.base (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('ve_before');		-- made before the extension

CREATE EXTENSION citext SCHEMA ve;
CREATE EXTENSION hstore SCHEMA ve;
CREATE EXTENSION btree_gist SCHEMA ve;
CREATE EXTENSION test_prolly_ext_table SCHEMA ve;
SET search_path = public, ve;

-- installing them changed nothing a commit holds
SELECT table_name, status FROM dolt_status() ORDER BY 1;

-- the members' rows are shared (branch 0); the schema's own table isn't
SELECT count(*) FILTER (WHERE typbranch <> 0) AS versioned_types,
	   count(*) FILTER (WHERE typbranch = 0) > 0 AS shared_types
  FROM pg_type WHERE typnamespace = 've'::regnamespace AND typname IN ('citext', 'hstore');
SELECT relname, relbranch = 0 AS shared, amname
  FROM pg_class c JOIN pg_am a ON a.oid = c.relam
 WHERE relnamespace = 've'::regnamespace AND relkind = 'r'
 ORDER BY relname;

-- versioned tables use them, on two branches
CREATE TABLE ve.t (id int PRIMARY KEY, name ve.citext, attrs ve.hstore,
				   during int4range,
				   EXCLUDE USING gist (id WITH =, during WITH &&));
INSERT INTO ve.t VALUES (1, 'Hi', 'a=>1', '[1,5)');
SELECT length(dolt_commit('-Am', 'with extensions')::text) AS committed;
SELECT dolt_checkout('ve_before');
SELECT 'x'::ve.citext = 'X'::ve.citext AS same_type_here;
CREATE TABLE ve.u (id int PRIMARY KEY, name ve.citext);
INSERT INTO ve.u VALUES (1, 'There');
SELECT dolt_checkout('-b', 've_after', 'main');
SELECT id, name, attrs -> 'a' AS a FROM ve.t;
INSERT INTO ve.t VALUES (2, 'Yo', 'a=>2', '[1,5)');
INSERT INTO ve.t VALUES (3, 'No', 'a=>3', '[2,3)');	-- the exclusion holds
SELECT length(dolt_commit('-Am', 'on after')::text) AS committed;
SELECT dolt_checkout('main');
SELECT length(dolt_merge('ve_after')::text) > 0 AS merged;
SELECT id, name FROM ve.t ORDER BY id;

-- the extension's table is the same on every branch, and never committed
INSERT INTO ve.test_prolly_ext_rows VALUES (10);
SELECT dolt_checkout('ve_before');
SELECT * FROM ve.test_prolly_ext_rows;
SELECT dolt_checkout('main');
SELECT table_name FROM dolt_status() ORDER BY 1;

-- refused while the extensions are in the schema
SET client_min_messages = warning;
DROP SCHEMA ve CASCADE;
SELECT dolt_checkout('ve_before');
DROP SCHEMA ve CASCADE;
SELECT dolt_checkout('main');
RESET client_min_messages;
ALTER SCHEMA ve RENAME TO ve2;
ALTER EXTENSION citext DROP FUNCTION ve.citext(boolean);
CREATE FUNCTION ve.mine() RETURNS int LANGUAGE sql RETURN 1;
ALTER EXTENSION citext ADD FUNCTION ve.mine();
CREATE SCHEMA plain_ext;
ALTER EXTENSION hstore SET SCHEMA plain_ext;
CREATE EXTENSION ltree SCHEMA plain_ext;
ALTER EXTENSION ltree SET SCHEMA ve;

-- a versioned schema only some branches have can't take an extension
CREATE SCHEMA ve_main_only VERSIONED;
CREATE EXTENSION cube SCHEMA ve_main_only;

-- dropping an extension still sees every branch's dependents
DROP TABLE ve.t;
SELECT length(dolt_commit('-Am', 'tables gone')::text) AS committed;
DROP EXTENSION citext;		-- ve_before's ve.u, ve_after's ve.t
SELECT dolt_branch('-D', 've_after');
SELECT dolt_checkout('ve_before');
DROP TABLE ve.u;
SELECT dolt_checkout('main');
DROP EXTENSION citext, hstore, btree_gist, test_prolly_ext_table;
DROP SCHEMA ve CASCADE;

\c contrib_regression
DROP DATABASE regress_versioned_extensions;

-- An index added to an extension's shared table later is shared too, so
-- every branch's writes keep it; and a script can't make an object whose
-- identity another branch's object has.
CREATE DATABASE regress_versioned_extensions2;
\c regress_versioned_extensions2
CREATE SCHEMA v VERSIONED;
CREATE TABLE v.base (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
CREATE EXTENSION test_prolly_ext_table SCHEMA v;
SELECT dolt_checkout('-b', 'side');
SELECT dolt_checkout('main');
CREATE UNIQUE INDEX ext_unique ON v.test_prolly_ext_rows (v);
SELECT relbranch = 0 AS shared FROM pg_class WHERE relname = 'ext_unique';
SELECT dolt_checkout('side');
INSERT INTO v.test_prolly_ext_rows VALUES (10);
SELECT dolt_checkout('main');
SET enable_seqscan = off;
SELECT * FROM v.test_prolly_ext_rows WHERE v = 10;
RESET enable_seqscan;
ALTER TABLE v.test_prolly_ext_rows ADD CONSTRAINT ext_small CHECK (v < 100);
SELECT dolt_checkout('side');
INSERT INTO v.test_prolly_ext_rows VALUES (500);	-- the constraint is shared
SELECT conname FROM pg_constraint WHERE conrelid = 'v.test_prolly_ext_rows'::regclass ORDER BY 1;

-- side's own type and function take names the extensions would make
SELECT dolt_checkout('main');
DROP EXTENSION test_prolly_ext_table;
SELECT dolt_checkout('side');
CREATE TYPE v.citext AS ENUM ('red');
CREATE FUNCTION v.test_prolly_ext_count() RETURNS bigint LANGUAGE sql RETURN 0;
SELECT dolt_checkout('main');
CREATE EXTENSION citext SCHEMA v;
CREATE EXTENSION test_prolly_ext_table SCHEMA v;
SELECT count(*) AS installed FROM pg_extension WHERE extname IN ('citext', 'test_prolly_ext_table');
SELECT dolt_checkout('side');
SELECT 'red'::v.citext AS sides_enum;
DROP TYPE v.citext;
DROP FUNCTION v.test_prolly_ext_count();
SELECT dolt_checkout('main');
CREATE EXTENSION citext SCHEMA v;		-- nothing in the way now
CREATE EXTENSION test_prolly_ext_table SCHEMA v;
SELECT dolt_checkout('side');
SET search_path = public, v;
SELECT 'Red'::v.citext = 'RED'::v.citext AS shared_citext, v.test_prolly_ext_count() AS rows;
SELECT dolt_checkout('main');

-- an extension's own scripts may let its objects go (cube's and seg's do,
-- installing and updating), in a versioned schema too; a user's ALTER
-- EXTENSION ... DROP there is still refused
CREATE EXTENSION cube VERSION '1.4' SCHEMA v;
ALTER EXTENSION cube UPDATE TO '1.5';
CREATE EXTENSION seg SCHEMA v;
SELECT extname, extversion FROM pg_extension WHERE extname IN ('cube', 'seg') ORDER BY 1;
SELECT count(*) FILTER (WHERE probranch <> 0) AS versioned_members
  FROM pg_proc p JOIN pg_depend d ON d.classid = 'pg_proc'::regclass AND d.objid = p.oid
 WHERE d.deptype = 'e' AND d.refobjid IN (SELECT oid FROM pg_extension WHERE extname IN ('cube', 'seg'));
ALTER EXTENSION seg DROP FUNCTION v.seg_size(v.seg);

\c contrib_regression
DROP DATABASE regress_versioned_extensions2;
