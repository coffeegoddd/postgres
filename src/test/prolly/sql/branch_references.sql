-- References between branches and shared schemas: versioned objects may
-- depend on shared ones, never the other way round, nor across branches;
-- a shared table's foreign keys from several branches each act on their
-- own branch's table.
SELECT pg_branch_id_allocate() AS b1 \gset
SELECT pg_branch_id_allocate() AS b2 \gset
CREATE SCHEMA pub;
CREATE TABLE pub.p (id int PRIMARY KEY);
INSERT INTO pub.p VALUES (1), (2), (3);
SET catalog_branch = :b1;
CREATE SCHEMA app VERSIONED;
CREATE TYPE app.mood AS ENUM ('ok');
CREATE TABLE app.c (id int PRIMARY KEY, p int REFERENCES pub.p ON DELETE CASCADE,
					r int REFERENCES pub.p ON DELETE RESTRICT);
INSERT INTO app.c VALUES (11, 1, NULL), (12, 2, NULL);
SELECT 'app.c'::regclass::oid AS c1 \gset
SET catalog_branch = :b2;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.c (id int PRIMARY KEY, p int REFERENCES pub.p ON DELETE CASCADE,
					r int REFERENCES pub.p ON DELETE RESTRICT);
INSERT INTO app.c VALUES (21, 1, NULL), (22, 2, 3);

-- a shared row deleted from one branch cascades on every branch's table
SET catalog_branch = :b1;
DELETE FROM pub.p WHERE id = 1;
SELECT id FROM app.c ORDER BY id;
SET catalog_branch = :b2;
SELECT id FROM app.c ORDER BY id;
-- and a row another branch's table restricts stays
SET catalog_branch = :b1;
DELETE FROM pub.p WHERE id = 3;
SELECT id FROM pub.p ORDER BY id;

-- a shared object cannot depend on a versioned one
CREATE VIEW pub.v AS SELECT * FROM app.c;
CREATE TABLE pub.x (m app.mood);
CREATE TABLE pub.y (c int REFERENCES app.c);
CREATE FUNCTION pub.f() RETURNS bigint LANGUAGE sql
	BEGIN ATOMIC SELECT count(*) FROM app.c; END;
-- nor a versioned one on another branch's
SET catalog_branch = :b2;
CREATE TABLE app.z (r regclass DEFAULT :'c1'::regclass);
-- but a temporary one may, being the session's
CREATE TEMP VIEW tv AS SELECT * FROM app.c;
SELECT count(*) FROM tv;
DROP VIEW tv;

-- objects do not move between kinds of schema, nor between branches
ALTER TABLE app.c SET SCHEMA pub;
ALTER TABLE pub.p SET SCHEMA app;
CREATE SCHEMA app2 VERSIONED;
ALTER TABLE app.c SET SCHEMA app2;
SELECT count(*) FROM app2.c;

-- default privileges in a versioned schema are its branch's
SET catalog_branch = :b1;
ALTER DEFAULT PRIVILEGES IN SCHEMA app GRANT SELECT ON TABLES TO PUBLIC;
ALTER DEFAULT PRIVILEGES IN SCHEMA app REVOKE SELECT ON TABLES FROM PUBLIC;

-- a cast from a shared type to a branch's type is that branch's
CREATE FUNCTION app.tomood(int) RETURNS app.mood LANGUAGE sql IMMUTABLE
	AS $$SELECT 'ok'::app.mood$$;
CREATE CAST (int AS app.mood) WITH FUNCTION app.tomood(int);
SELECT 1::app.mood;
DROP CAST (int AS app.mood);

-- a branch's table cannot inherit from, or be a partition of, a shared one
RESET catalog_branch;
CREATE TABLE pub.ip (a int);
CREATE TABLE pub.parent (a int) PARTITION BY LIST (a);
SET catalog_branch = :b1;
CREATE TABLE app.ch () INHERITS (pub.ip);
CREATE TABLE app.part PARTITION OF pub.parent FOR VALUES IN (1);
CREATE TABLE app.loose (a int);
ALTER TABLE app.loose INHERIT pub.ip;
ALTER TABLE pub.parent ATTACH PARTITION app.loose FOR VALUES IN (2);
-- one branch's tables may, and a temporary table may inherit from a shared one
CREATE TABLE app.bparent (a int) PARTITION BY LIST (a);
CREATE TABLE app.bpart PARTITION OF app.bparent FOR VALUES IN (1);
CREATE TEMP TABLE tch () INHERITS (pub.ip);
DROP TABLE tch;

-- detaching a partition of a shared referenced table checks each branch's
-- referencing table, whichever branch the session is on
RESET catalog_branch;
CREATE TABLE pub.pp (id int PRIMARY KEY) PARTITION BY LIST (id);
CREATE TABLE pub.pp1 PARTITION OF pub.pp FOR VALUES IN (1);
INSERT INTO pub.pp VALUES (1);
SET catalog_branch = :b1;
CREATE TABLE app.r (p int REFERENCES pub.pp);
INSERT INTO app.r VALUES (1);
SET catalog_branch = :b2;
CREATE TABLE app.r (p int);
ALTER TABLE pub.pp DETACH PARTITION pub.pp1;
SET catalog_branch = :b1;
DROP TABLE app.r;

-- a shared table's column type changed on no branch rebuilds each branch's
-- foreign keys into it, which its queries don't see, and checks them on
-- their own branch's tables
RESET catalog_branch;
ALTER TABLE pub.p ALTER COLUMN id TYPE bigint;
SET catalog_branch = :b1;
SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint
WHERE confrelid = 'pub.p'::regclass ORDER BY 1;
INSERT INTO app.c VALUES (13, 99, NULL);
SET catalog_branch = :b2;
SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint
WHERE confrelid = 'pub.p'::regclass ORDER BY 1;

SET catalog_branch = :b2;
SET client_min_messages = warning;
DROP SCHEMA app, app2 CASCADE;
SET catalog_branch = :b1;
DROP SCHEMA app CASCADE;
RESET catalog_branch;
DROP SCHEMA pub CASCADE;
RESET client_min_messages;
