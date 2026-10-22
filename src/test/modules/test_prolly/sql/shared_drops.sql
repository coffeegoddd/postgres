-- A shared object (outside the versioned schemas: an extension, an
-- ordinary schema's type, function or table) is every branch's.  Dropping
-- one while an object on another branch depends on it is refused, CASCADE
-- or not: CASCADE reaches only the session's branch, and the dependent
-- would be left naming an object that is gone.  Dependents on the
-- session's own branch behave as on stock Postgres.

CREATE SCHEMA sd_ext;
CREATE EXTENSION citext SCHEMA sd_ext;
CREATE DOMAIN public.sd_pos AS int CHECK (VALUE > 0);
CREATE FUNCTION public.sd_one() RETURNS int LANGUAGE sql RETURN 1;
CREATE TABLE public.sd_plain (id int PRIMARY KEY, a int, b int);
CREATE SCHEMA sd VERSIONED;
CREATE TABLE sd.base (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('sd_side');

-- on sd_side only: a column of an extension type, a column of a shared
-- domain with a shared function as its default, a view over a shared
-- table's column
SELECT dolt_checkout('sd_side');
CREATE TABLE sd.t (id int PRIMARY KEY, name sd_ext.citext,
				   n public.sd_pos DEFAULT public.sd_one());
INSERT INTO sd.t (id, name) VALUES (1, 'Hi');
CREATE VIEW sd.v AS SELECT id, a FROM public.sd_plain;
SELECT dolt_checkout('main');

-- each drop is refused from main, with CASCADE too
DROP EXTENSION citext;
DROP EXTENSION citext CASCADE;
DROP SCHEMA sd_ext CASCADE;
DROP DOMAIN public.sd_pos;
DROP DOMAIN public.sd_pos CASCADE;
DROP FUNCTION public.sd_one();
DROP TABLE public.sd_plain;
ALTER TABLE public.sd_plain DROP COLUMN a;
ALTER TABLE public.sd_plain DROP COLUMN a CASCADE;
-- a column nothing on sd_side uses can go
ALTER TABLE public.sd_plain DROP COLUMN b;

-- sd_side still reads and writes
SELECT dolt_checkout('sd_side');
SELECT id, name, n FROM sd.t ORDER BY id;
INSERT INTO sd.t (id, name) VALUES (2, 'Yo');
SELECT id, name, n FROM sd.t ORDER BY id;
SELECT * FROM sd.v;

-- dependents on two other branches
SELECT dolt_branch('sd_third');
SELECT dolt_checkout('sd_third');
CREATE TABLE sd.t3 (id int PRIMARY KEY, name sd_ext.citext);
SELECT dolt_checkout('main');
DROP EXTENSION citext;
SELECT dolt_checkout('sd_third');
DROP TABLE sd.t3;

-- the other way round: from sd_side, a dependent on main refuses too
SELECT dolt_checkout('main');
CREATE TABLE sd.m (id int PRIMARY KEY, n public.sd_pos);
SELECT dolt_checkout('sd_side');
DROP TABLE sd.t;
DROP DOMAIN public.sd_pos;
SELECT dolt_checkout('main');

-- dependents on the session's own branch: stock RESTRICT and CASCADE
CREATE TABLE sd.own (id int PRIMARY KEY, name sd_ext.citext);
DROP FUNCTION public.sd_one();	-- sd_side dropped sd.t, its only user
DROP TABLE sd.m;
DROP DOMAIN public.sd_pos;		-- sd.m and sd_side's sd.t, its users, are gone
SELECT dolt_checkout('sd_side');
DROP VIEW sd.v;
SELECT dolt_checkout('main');
DROP TABLE public.sd_plain;
DROP EXTENSION citext;			-- RESTRICT: sd.own's column depends on it
SET client_min_messages = warning;
DROP EXTENSION citext CASCADE;	-- drops sd.own's column, on main only
RESET client_min_messages;
SELECT attname FROM pg_attribute
 WHERE attrelid = 'sd.own'::regclass AND attnum > 0 AND NOT attisdropped
 ORDER BY attnum;

SELECT dolt_branch('-D', 'sd_side');
SELECT dolt_branch('-D', 'sd_third');
DROP SCHEMA sd CASCADE;
DROP SCHEMA sd_ext;

-- the session's branch has no versioned schema left, another branch still
-- does: its dependents still refuse the drop (in a database of its own, so
-- no other test's versioned schemas are on main)
CREATE DATABASE regress_shared_drops;
\c regress_shared_drops
CREATE DOMAIN public.sd_pos2 AS int CHECK (VALUE > 0);
CREATE SCHEMA sd2 VERSIONED;
CREATE TABLE sd2.t (id int PRIMARY KEY, n public.sd_pos2);
INSERT INTO sd2.t VALUES (1, 1);
SELECT length(dolt_commit('-Am', 'sd2')::text) AS committed;
SELECT dolt_checkout('-b', 'sd_left');
SELECT dolt_checkout('main');
SET client_min_messages = warning;
DROP SCHEMA sd2 CASCADE;
RESET client_min_messages;
SELECT count(*) AS versioned_here FROM pg_namespace WHERE nspversioned;
DROP DOMAIN public.sd_pos2 CASCADE;
SELECT dolt_checkout('sd_left');
SELECT * FROM sd2.t;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'sd_left');
DROP DOMAIN public.sd_pos2;
\c contrib_regression
DROP DATABASE regress_shared_drops;
