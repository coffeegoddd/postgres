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

SET client_min_messages = warning;
DROP SCHEMA app, app2 CASCADE;
SET catalog_branch = :b1;
DROP SCHEMA app CASCADE;
RESET catalog_branch;
DROP SCHEMA pub CASCADE;
RESET client_min_messages;
