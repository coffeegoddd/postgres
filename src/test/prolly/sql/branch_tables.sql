-- What a versioned schema's tables can't be: a column of a type naming an object, however it's
-- made, or a typed table.  Each is refused as its transaction commits.  A shared schema's tables
-- can be either.
SELECT pg_branch_id_allocate() AS b1 \gset
SET catalog_branch = :b1;
CREATE SCHEMA app VERSIONED;
CREATE SCHEMA pub;
SET search_path = app, pub;

-- a column of each type naming an object
CREATE TABLE t (id int PRIMARY KEY, r regclass);
CREATE TABLE t (id int PRIMARY KEY, r regtype);
CREATE TABLE t (id int PRIMARY KEY, r regproc);
CREATE TABLE t (id int PRIMARY KEY, r regprocedure);
CREATE TABLE t (id int PRIMARY KEY, r regoper);
CREATE TABLE t (id int PRIMARY KEY, r regoperator);
CREATE TABLE t (id int PRIMARY KEY, r regconfig);
CREATE TABLE t (id int PRIMARY KEY, r regdictionary);
CREATE TABLE t (id int PRIMARY KEY, r regnamespace);
CREATE TABLE t (id int PRIMARY KEY, r regrole);
CREATE TABLE t (id int PRIMARY KEY, r regcollation);
CREATE TABLE t (id int PRIMARY KEY, r aclitem);

-- inside an array, a domain, a composite type, and those in each other
CREATE TABLE t (id int PRIMARY KEY, r regclass[]);
CREATE DOMAIN app.names_a_class AS regclass;
CREATE TABLE t (id int PRIMARY KEY, r app.names_a_class);
CREATE TYPE app.holds_a_role AS (who regrole, n int);
CREATE TABLE t (id int PRIMARY KEY, r app.holds_a_role);
CREATE TYPE app.holds_holder AS (inner_one app.holds_a_role);
CREATE TABLE t (id int PRIMARY KEY, r app.holds_holder[]);
CREATE DOMAIN app.many_names AS app.names_a_class[];
CREATE TABLE t (id int PRIMARY KEY, r app.many_names);

-- made any way at all: added, retyped, from a query, or on a partitioned table and so its partitions
-- (moving a table into a versioned schema is refused already)
CREATE TABLE t (id int PRIMARY KEY, v text);
ALTER TABLE t ADD COLUMN r regclass;
ALTER TABLE t ALTER COLUMN v TYPE regrole USING NULL;
CREATE TABLE t2 AS SELECT 'pg_class'::regclass AS r;
CREATE TABLE pub.moved (id int PRIMARY KEY, r regclass);
ALTER TABLE pub.moved SET SCHEMA app;
CREATE TABLE parts (id int PRIMARY KEY, r text) PARTITION BY RANGE (id);
CREATE TABLE parts1 PARTITION OF parts FOR VALUES FROM (0) TO (10);
ALTER TABLE parts ADD COLUMN extra regtype;
BEGIN;
CREATE TABLE in_a_transaction (r regclass);
SAVEPOINT s;
CREATE TABLE fine (v text);
COMMIT;
SELECT to_regclass('app.fine') IS NULL AS not_made_either;

-- one made and gone in the transaction is nothing to refuse
BEGIN;
CREATE TABLE gone (r regclass);
DROP TABLE gone;
COMMIT;

-- a typed table
CREATE TYPE app.row_type AS (id int, v text);
CREATE TABLE typed OF app.row_type;

-- on another branch than the session's, too
SELECT pg_branch_id_allocate() AS b2 \gset
SET catalog_branch = :b2;
CREATE SCHEMA app VERSIONED;
SET catalog_branch = :b1;
BEGIN;
SET LOCAL catalog_branch = :b2;
CREATE TABLE app.on_b2 (r regclass);
SET LOCAL catalog_branch = :b1;
COMMIT;

-- what is fine: a shared schema's, other columns, and what's only like these
CREATE TABLE pub.anything (id int PRIMARY KEY, r regclass, a aclitem[]);
CREATE TABLE ok (id int PRIMARY KEY, o oid, n name, t text, j jsonb);
SELECT count(*) AS tables FROM pg_class WHERE relnamespace = 'app'::regnamespace AND relkind IN ('r', 'p');

DROP TABLE pub.anything;
DROP SCHEMA pub CASCADE;
DROP SCHEMA app CASCADE;
SET catalog_branch = :b2;
DROP SCHEMA app CASCADE;
