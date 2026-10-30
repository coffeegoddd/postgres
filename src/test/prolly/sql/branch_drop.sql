-- Dropping a branch's catalog rows: every versioned schema of the branch
-- and everything in it go, and every other branch's rows and the shared
-- ones stay.
SELECT pg_branch_id_allocate() AS b1 \gset
SELECT pg_branch_id_allocate() AS b2 \gset
CREATE SCHEMA pub;
CREATE TABLE pub.p (id int PRIMARY KEY);
INSERT INTO pub.p VALUES (1);
CREATE EXTENSION hstore SCHEMA pub;

SET catalog_branch = :b1;
CREATE SCHEMA app VERSIONED;
CREATE TYPE app.mood AS ENUM ('ok');
CREATE TABLE app.t (id int PRIMARY KEY, p int REFERENCES pub.p, m app.mood,
					h pub.hstore);
CREATE VIEW app.v AS SELECT id FROM app.t;
CREATE FUNCTION app.f() RETURNS int LANGUAGE sql RETURN 1;
INSERT INTO app.t VALUES (1, 1, 'ok', 'a=>1');
SET catalog_branch = :b2;
CREATE SCHEMA app VERSIONED;
CREATE TYPE app.mood AS ENUM ('ok');
CREATE TABLE app.t (id int PRIMARY KEY, p int REFERENCES pub.p, m app.mood,
					h pub.hstore);
CREATE VIEW app.v AS SELECT id FROM app.t;
CREATE FUNCTION app.f() RETURNS int LANGUAGE sql RETURN 2;
CREATE SCHEMA app2 VERSIONED CREATE TABLE u (x text);
CREATE EXTENSION pg_trgm SCHEMA app2;
INSERT INTO app.t VALUES (2, 1, 'ok', 'b=>2');

-- what each branch has, counted with the filter off
SET catalog_branch_filter = off;
CREATE TEMP VIEW branch_rows AS
SELECT n.nspbranch AS branch, count(*) AS objects
FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspbranch IN (:b1, :b2)
GROUP BY n.nspbranch;
SELECT branch = :b1 AS b1, objects FROM branch_rows ORDER BY branch;
SELECT count(*) AS triggers_on_pub_p FROM pg_trigger WHERE tgrelid = 'pub.p'::regclass;
RESET catalog_branch_filter;

-- drop b2's, from a session on b1
SET catalog_branch = :b1;
SELECT pg_branch_catalog_drop(:b2);

SET catalog_branch_filter = off;
SELECT branch = :b1 AS b1, objects FROM branch_rows ORDER BY branch;
SELECT count(*) AS triggers_on_pub_p FROM pg_trigger WHERE tgrelid = 'pub.p'::regclass;
SELECT count(*) AS b2_namespaces FROM pg_namespace WHERE nspbranch = :b2;
SELECT count(*) AS b2_extensions FROM pg_extension WHERE extname = 'pg_trgm';
RESET catalog_branch_filter;

-- b1 and the shared schema are as they were
SELECT * FROM app.t;
SELECT app.f(), (SELECT count(*) FROM app.v) AS view_rows;
SELECT * FROM pub.p;
SELECT count(*) AS hstore FROM pg_extension WHERE extname = 'hstore';
SET catalog_branch = :b2;
SELECT * FROM app.t;

-- the branch can have schemas again, and dropping a branch with none, or
-- dropping again, does nothing
CREATE SCHEMA app VERSIONED;
SELECT pg_branch_catalog_drop(:b2);
SELECT pg_branch_catalog_drop(:b2);
SELECT pg_branch_catalog_drop(0);

-- only the superuser may
CREATE ROLE regress_branch_dropper;
SET ROLE regress_branch_dropper;
SELECT pg_branch_catalog_drop(:b1);
RESET ROLE;
DROP ROLE regress_branch_dropper;

SELECT pg_branch_catalog_drop(:b1);
RESET catalog_branch;
DROP SCHEMA pub CASCADE;
