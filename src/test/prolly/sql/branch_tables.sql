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
CREATE TABLE t (id int PRIMARY KEY, r regdatabase);
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
CREATE TYPE app.class_range AS RANGE (SUBTYPE = regclass, SUBTYPE_OPCLASS = oid_ops);
CREATE TABLE t (id int PRIMARY KEY, r app.class_range);
CREATE TABLE t (id int PRIMARY KEY, r app.class_multirange);

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

-- in a savepoint released, and in one rolled back
BEGIN;
SAVEPOINT s;
CREATE TABLE in_savepoint (r regclass);
RELEASE s;
COMMIT;
BEGIN;
SAVEPOINT s;
CREATE TABLE rolled_back (r regclass);
ROLLBACK TO s;
COMMIT;

-- one made and gone in the transaction is nothing to refuse
BEGIN;
CREATE TABLE gone (r regclass);
DROP TABLE gone;
COMMIT;

-- a type changed under a versioned table's column: a composite, its own or a shared one, through a
-- domain, a shared table's row type, and a view's
CREATE TYPE app.later AS (n int);
CREATE TABLE uses_later (id int PRIMARY KEY, c app.later);
ALTER TYPE app.later ADD ATTRIBUTE r regclass;
CREATE TYPE pub.shared_later AS (n int);
CREATE TABLE uses_shared (id int PRIMARY KEY, c pub.shared_later[]);
ALTER TYPE pub.shared_later ADD ATTRIBUTE r regclass CASCADE;
CREATE TYPE pub.under_domain AS (n int);
CREATE DOMAIN pub.over_it AS pub.under_domain;
CREATE TABLE uses_domain (id int PRIMARY KEY, c pub.over_it);
ALTER TYPE pub.under_domain ADD ATTRIBUTE r regrole;
CREATE TYPE pub.nested_inner AS (n int);
CREATE TYPE pub.nested_outer AS (inner_one pub.nested_inner);
CREATE TABLE uses_nested (id int PRIMARY KEY, c pub.nested_outer);
ALTER TYPE pub.nested_inner ADD ATTRIBUTE r regclass;
CREATE TYPE pub.ranged_inner AS (n int);
CREATE TYPE pub.ranged AS RANGE (SUBTYPE = pub.ranged_inner, MULTIRANGE_TYPE_NAME = pub.ranged_many);
CREATE TABLE uses_ranged (id int PRIMARY KEY, c pub.ranged_many);
ALTER TYPE pub.ranged_inner ADD ATTRIBUTE r regclass;
CREATE FOREIGN DATA WRAPPER no_handler;
CREATE SERVER nowhere FOREIGN DATA WRAPPER no_handler;
CREATE FOREIGN TABLE pub.its_foreign (n int) SERVER nowhere;
CREATE TABLE uses_foreign (id int PRIMARY KEY, c pub.its_foreign);
ALTER FOREIGN TABLE pub.its_foreign ADD COLUMN r regclass;
CREATE TABLE pub.its_row (n int);
CREATE TABLE uses_row (id int PRIMARY KEY, c pub.its_row);
ALTER TABLE pub.its_row ADD COLUMN r regclass;
CREATE VIEW pub.its_view AS SELECT 1 AS n;
CREATE TABLE uses_view (id int PRIMARY KEY, c pub.its_view);
CREATE OR REPLACE VIEW pub.its_view AS SELECT 1 AS n, 'pg_class'::regclass AS r;
SELECT string_agg(relname, ',' ORDER BY relname) AS still_as_they_were FROM pg_class
WHERE relnamespace = 'app'::regnamespace AND relname LIKE 'uses_%' AND relkind = 'r';

-- a chain of types longer than a walk of them by recursion would have room for
CREATE TYPE pub.chain_root AS (n int);
DO $$
DECLARE
	under text := 'pub.chain_root';
BEGIN
	FOR i IN 1 .. 100000 LOOP
		EXECUTE format('CREATE DOMAIN pub.chain_%s AS %s', i, under);
		under := format('pub.chain_%s', i);
		IF i % 1000 = 0 THEN
			COMMIT;
		END IF;
	END LOOP;
END
$$;
ALTER TYPE pub.chain_root ADD ATTRIBUTE m text;
SELECT count(*) AS still_up FROM pg_type WHERE typname = 'chain_100000';

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
DROP SERVER nowhere CASCADE;
DROP FOREIGN DATA WRAPPER no_handler;
DROP SCHEMA pub CASCADE;
DROP SCHEMA app CASCADE;
SET catalog_branch = :b2;
DROP SCHEMA app CASCADE;
