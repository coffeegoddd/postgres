-- A change to a shared object (outside the versioned schemas) that checks
-- or rebuilds what depends on it can't reach another branch's dependents:
-- it is refused while there are any.  A domain constraint can't be checked
-- against another branch's columns, a column whose type changes can't have
-- another branch's views rebuilt, a composite type's attribute can't change
-- type under another branch's columns, and another branch's typed tables
-- can't be altered with their type.  Changes stock Postgres makes without
-- looking at dependents still go through, and dependents on the session's
-- own branch behave as on stock Postgres.  (In a database of its own, so
-- the branches here are the only ones.)

CREATE DATABASE regress_shared_alters;
\c regress_shared_alters

CREATE DOMAIN public.sa_d AS int;
CREATE DOMAIN public.sa_fn_only AS int;
CREATE TABLE public.sa_s (id int, a int, b int);
CREATE TYPE public.sa_comp AS (x int, y int);
CREATE TYPE public.sa_typed AS (x int);
CREATE TABLE public.sa_row (id int, v int);
CREATE SCHEMA sa VERSIONED;
CREATE TABLE sa.base (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('sa_side');

-- on sa_side only
SELECT dolt_checkout('sa_side');
CREATE TABLE sa.t (id int PRIMARY KEY, n public.sa_d, c public.sa_comp,
				   r public.sa_row);
INSERT INTO sa.t VALUES (1, -5, ROW(1, 2), ROW(1, 1));
CREATE VIEW sa.v AS SELECT id, a FROM public.sa_s;
CREATE TABLE sa.tt OF public.sa_typed (PRIMARY KEY (x));
CREATE FUNCTION sa.f(public.sa_fn_only) RETURNS int LANGUAGE sql RETURN 1;
SELECT dolt_checkout('main');

-- domains: no constraint can be checked against sa_side's column
ALTER DOMAIN public.sa_d ADD CONSTRAINT pos CHECK (VALUE > 0);
ALTER DOMAIN public.sa_d SET NOT NULL;
ALTER DOMAIN public.sa_d ADD CONSTRAINT pos CHECK (VALUE > 0) NOT VALID;	-- checks nothing
ALTER DOMAIN public.sa_d VALIDATE CONSTRAINT pos;
-- a function's argument holds no stored value: this one is checked here
ALTER DOMAIN public.sa_fn_only ADD CONSTRAINT pos CHECK (VALUE > 0);

-- a shared table's column under sa_side's view, and its row type in
-- sa_side's column
ALTER TABLE public.sa_s ALTER COLUMN a TYPE text;
ALTER TABLE public.sa_s ALTER COLUMN b TYPE text;	-- nothing uses b
ALTER TABLE public.sa_row ALTER COLUMN v TYPE bigint;

-- composite types: an attribute's type under sa_side's column, a typed
-- table on sa_side; adding an attribute doesn't look at columns
ALTER TYPE public.sa_comp ALTER ATTRIBUTE y TYPE bigint;
ALTER TYPE public.sa_comp ADD ATTRIBUTE z int;
ALTER TYPE public.sa_typed ADD ATTRIBUTE y int;
ALTER TYPE public.sa_typed ADD ATTRIBUTE y int CASCADE;

-- sa_side is as it was
SELECT dolt_checkout('sa_side');
SELECT id, n, c, r FROM sa.t;
SELECT * FROM sa.v;
\d sa.v
\d sa.tt

-- from sa_side, main's dependents refuse too
SELECT dolt_checkout('main');
CREATE TABLE sa.m (id int PRIMARY KEY, n public.sa_d);
SELECT dolt_checkout('sa_side');
DROP TABLE sa.t, sa.tt;
DROP VIEW sa.v;
ALTER DOMAIN public.sa_d SET NOT NULL;
SELECT dolt_checkout('main');

-- with sa_side's dependents gone: stock behaviour on the session's branch
DROP TABLE sa.m;
ALTER DOMAIN public.sa_d DROP CONSTRAINT pos;	-- the NOT VALID one
CREATE TABLE sa.own (id int PRIMARY KEY, n public.sa_d);
INSERT INTO sa.own VALUES (1, -5);
ALTER DOMAIN public.sa_d ADD CONSTRAINT pos CHECK (VALUE > 0);	-- fails on main's row
DELETE FROM sa.own;
ALTER DOMAIN public.sa_d ADD CONSTRAINT pos CHECK (VALUE > 0);
ALTER TABLE public.sa_s ALTER COLUMN a TYPE text;
ALTER TYPE public.sa_comp ALTER ATTRIBUTE y TYPE bigint;
ALTER TABLE public.sa_row ALTER COLUMN v TYPE bigint;
ALTER TYPE public.sa_typed ADD ATTRIBUTE y int;

\c contrib_regression
DROP DATABASE regress_shared_alters;
