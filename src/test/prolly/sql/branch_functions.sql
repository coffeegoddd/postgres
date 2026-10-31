-- Functions and branches: what a function's names mean is fixed by where
-- it lives, never by the branch of whoever calls it.
SELECT pg_branch_id_allocate() AS b1 \gset
SELECT pg_branch_id_allocate() AS b2 \gset
CREATE SCHEMA pubf;

-- the same versioned table on two branches, its columns in other orders
SET catalog_branch = :b1;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int, v int);
INSERT INTO app.t VALUES (1, 10);
CREATE TABLE app.allowed (name text);
INSERT INTO app.allowed VALUES ('alice');
SET catalog_branch = :b2;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (v int, id int);
INSERT INTO app.t VALUES (20, 2);

-- a function in a versioned schema runs on its schema's branch: a default
-- of a temporary table, made on b1, used on b2, reads b1's app.t
SET catalog_branch = :b1;
CREATE FUNCTION app.max_v() RETURNS int LANGUAGE sql STABLE
	AS 'SELECT max(v) FROM app.t';
CREATE TEMP TABLE tt (x int DEFAULT app.max_v());
SET catalog_branch = :b2;
INSERT INTO tt DEFAULT VALUES;
SELECT x FROM tt;

-- nor is it inlined where it would read the session's branch: an
-- expression's function, and a set-returning one in FROM
SET catalog_branch = :b1;
CREATE FUNCTION app.base() RETURNS int LANGUAGE sql IMMUTABLE AS 'SELECT 1';
CREATE FUNCTION app.plus(int) RETURNS int LANGUAGE sql IMMUTABLE
	AS 'SELECT $1 + app.base()';
CREATE FUNCTION app.rows() RETURNS SETOF int LANGUAGE sql STABLE
	AS 'SELECT v FROM app.t';
SET catalog_branch = :b2;
CREATE FUNCTION app.base() RETURNS int LANGUAGE sql IMMUTABLE AS 'SELECT 2';
SET catalog_branch = :b1;
CREATE TEMP VIEW ti AS SELECT app.plus(g) AS x FROM generate_series(10, 10) g;
CREATE TEMP VIEW tv AS SELECT * FROM app.rows() r;
SET catalog_branch = :b2;
SELECT x FROM ti;
SELECT * FROM tv;

-- a function that runs on its own branch keeps the caller's sequence state
SET catalog_branch = :b2;
CREATE SEQUENCE app.seq;
SELECT nextval('app.seq');
INSERT INTO tt DEFAULT VALUES;
SELECT currval('app.seq'), lastval();

-- a shared security definer function runs on no branch, whatever the
-- caller's, unless it sets catalog_branch itself: a caller can't make its
-- names mean another branch's objects, or none, and its own temporary
-- table in their place
SET catalog_branch = :b1;
CREATE FUNCTION pubf.allowed_unpinned(u text) RETURNS bool
	LANGUAGE sql SECURITY DEFINER SET search_path = app, pg_temp
	AS 'SELECT EXISTS (SELECT 1 FROM allowed WHERE name = u)';
SELECT pubf.allowed_unpinned('alice');
CREATE FUNCTION pubf.allowed(u text) RETURNS bool
	LANGUAGE sql SECURITY DEFINER SET search_path = app, pg_temp
	SET catalog_branch = :b1
	AS 'SELECT EXISTS (SELECT 1 FROM allowed WHERE name = u)';
RESET catalog_branch;
CREATE TEMP TABLE allowed (name text);
INSERT INTO allowed VALUES ('mallory');
SELECT pubf.allowed('alice'), pubf.allowed('mallory');
SET catalog_branch = :b2;
SELECT pubf.allowed('alice'), pubf.allowed('mallory');
DROP TABLE pg_temp.allowed;

-- a shared function is compiled for each branch it runs on: its row type
-- is that branch's app.t
CREATE FUNCTION pubf.first_v() RETURNS int LANGUAGE plpgsql AS $$
DECLARE
	r app.t%ROWTYPE;
BEGIN
	SELECT * INTO r FROM app.t;
	RETURN r.v;
END $$;
SET catalog_branch = :b1;
SELECT pubf.first_v();
SET catalog_branch = :b2;
SELECT pubf.first_v();
SET catalog_branch = :b1;
SELECT pubf.first_v();

DROP SCHEMA app CASCADE;
SET catalog_branch = :b2;
DROP SCHEMA app CASCADE;
RESET catalog_branch;
DROP SCHEMA pubf CASCADE;
