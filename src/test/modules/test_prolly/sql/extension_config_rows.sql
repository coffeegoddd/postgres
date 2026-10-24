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

-- A configuration table of values whose text forms follow the session's
-- settings (recorded in canonical form, so the same whatever they are),
-- with an identity, a generated column, row security hiding its rows from
-- everyone but its owner, more rows than any one array element limit,
-- and a configuration sequence moved from its start.
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/extconfig2-' || txid_current() AS url2 \gset
CREATE DATABASE regress_extconfig_src2;
\c regress_extconfig_src2
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
CREATE SCHEMA ec2 VERSIONED;
CREATE EXTENSION test_prolly_ext_config SCHEMA ec2;
CREATE TABLE ec2.t (id int PRIMARY KEY, n bigint DEFAULT ec2.test_prolly_cfg_count());
INSERT INTO ec2.test_prolly_cfg (at, day, ratio)
	VALUES ('2021-06-01 12:00 UTC', '2021-06-01', 0.1);
INSERT INTO ec2.test_prolly_cfg (at, day, ratio)
	SELECT '2022-01-01 00:00 UTC'::timestamptz + g * interval '1 hour', '2022-01-01'::date + g, g / 3.0
	FROM generate_series(1, 2500) g;
SELECT count(*) AS moved FROM (SELECT nextval('ec2.test_prolly_cfg_seq') FROM generate_series(1, 5)) s;
INSERT INTO ec2.t (id) VALUES (1);
-- committed by a role the table's policy hides every row from, under
-- other settings
CREATE ROLE regress_cfg_committer;
GRANT USAGE ON SCHEMA ec2 TO regress_cfg_committer;
GRANT ALL ON ec2.t TO regress_cfg_committer;
SET ROLE regress_cfg_committer;
SET DateStyle = 'SQL, DMY';
SET TimeZone = 'Asia/Kolkata';
SET extra_float_digits = -3;
SELECT has_table_privilege('ec2.test_prolly_cfg', 'SELECT') AS can_read;
SELECT (dolt_commit('-Am', 'configured'))[1] AS h1 \gset
RESET ROLE;
-- every row recorded (2501 of them, the script's not), and the sequence
SELECT (SELECT count(*) FROM regexp_matches(d, 'config\t', 'g')) AS rows_recorded,
       (SELECT count(*) FROM regexp_matches(d, 'configseq\t', 'g')) AS sequences_recorded
FROM (SELECT test_commit_dependencies(dolt_hashof('HEAD'))::text AS d) s;
SELECT md5(test_commit_dependencies(dolt_hashof('HEAD'))::text) AS deps_other \gset
RESET DateStyle;
RESET TimeZone;
RESET extra_float_digits;
-- recorded the same under the defaults
UPDATE ec2.t SET n = n + 0;
SELECT (dolt_commit('--allow-empty', '-am', 'same rows'))[1] AS h2 \gset
SELECT md5(test_commit_dependencies(dolt_hashof('HEAD'))::text) = :'deps_other' AS same_dependencies;
SELECT (dolt_reset('--hard', :'h1'))[1] AS reset_under_other_settings;
SELECT dolt_remote('add', 'origin', :'url2') IS NOT NULL AS remote;
SELECT dolt_push('origin', 'main') IS NOT NULL AS pushed;

\c :db
CREATE DATABASE regress_extconfig_clone2;
\c regress_extconfig_clone2
SET TimeZone = 'UTC';
SELECT dolt_clone(:'url2') IS NOT NULL AS cloned;
SELECT count(*) AS rows, min(id), max(id), sum(doubled) = sum(id) * 2 AS generated
FROM ec2.test_prolly_cfg;
SELECT at, day, ratio FROM ec2.test_prolly_cfg WHERE id = 2;
SELECT last_value, is_called FROM ec2.test_prolly_cfg_seq;
SELECT n FROM ec2.t;

\c :db
DROP DATABASE regress_extconfig_clone2;
DROP DATABASE regress_extconfig_src2;
DROP ROLE regress_cfg_committer;
