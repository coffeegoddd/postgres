--
-- An extension's configuration rows (the rows of its configuration tables
-- pg_dump dumps with it: a user's, by its extcondition) are recorded in
-- the commits' dependency on it.  A clone writes them into the extension
-- it makes, as a restore of a dump would; a move to a commit recording
-- rows the database lacks is refused, naming the table; rows the database
-- has besides are its own.  An extension without such rows is recorded as
-- before.  (test_prolly_ext_table's configuration table dumps v > 1.)
--
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT current_database() AS db \gset
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/extconfig-' || txid_current() AS url \gset
CREATE DATABASE regress_extconfig_src;
\c regress_extconfig_src
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
CREATE SCHEMA ec VERSIONED;
CREATE EXTENSION test_prolly_ext_table SCHEMA ec;
CREATE TABLE ec.t (id int PRIMARY KEY, n bigint DEFAULT ec.test_prolly_ext_count());
INSERT INTO ec.t (id) VALUES (1);
SELECT (dolt_commit('-Am', 'no configuration rows'))[1] AS c0 \gset
-- the script's own row (v = 1) isn't dumped, so isn't recorded
SELECT test_commit_dependencies(dolt_hashof('HEAD')) AS without_rows;

INSERT INTO ec.test_prolly_ext_rows VALUES (7), (9);
UPDATE ec.t SET n = ec.test_prolly_ext_count();
SELECT (dolt_commit('-am', 'with configuration rows'))[1] AS c1 \gset
SELECT unnest(test_commit_dependencies(dolt_hashof('HEAD'))) AS with_rows;
-- (not the cached dependencies of the commit before: the rows aren't
-- catalogs)
SELECT dolt_remote('add', 'origin', :'url') IS NOT NULL AS remote;
SELECT dolt_push('origin', 'main') IS NOT NULL AS pushed;

-- a move to a commit recording a row the database lacks: refused
DELETE FROM ec.test_prolly_ext_rows WHERE v = 9;
SELECT (dolt_reset('--hard', :'c0'))[1] AS back_to_c0;
SELECT dolt_reset('--hard', :'c1');
SELECT dolt_hashof('HEAD') = :'c0' AS still_at_c0;
-- with the row back, and one of the database's own besides
INSERT INTO ec.test_prolly_ext_rows VALUES (9), (11);
SELECT (dolt_reset('--hard', :'c1'))[1] AS to_c1;
SELECT n FROM ec.t;

-- a clone makes the extension and writes the recorded rows into it
\c :db
CREATE DATABASE regress_extconfig_clone;
\c regress_extconfig_clone
SELECT dolt_clone(:'url') IS NOT NULL AS cloned;
SELECT v FROM ec.test_prolly_ext_rows ORDER BY v;
SELECT n, ec.test_prolly_ext_count() AS counted FROM ec.t;

\c :db
DROP DATABASE regress_extconfig_clone;
DROP DATABASE regress_extconfig_src;
