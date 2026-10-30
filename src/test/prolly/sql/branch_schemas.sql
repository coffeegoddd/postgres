-- Versioned schemas: one pg_namespace row per branch, all of one name, and
-- a session that sees its branch's row, or a shared one.  Branch numbers
-- depend on what the database allocated before, so the test keeps them in
-- variables and shows only comparisons with them.
SELECT pg_branch_id_allocate() AS b1 \gset
SELECT pg_branch_id_allocate() AS b2 \gset
SELECT :b2 > :b1 AS allocated_in_order;

-- a session on no branch cannot make a versioned schema
SHOW catalog_branch;
CREATE SCHEMA app VERSIONED;

-- nor on a branch that was never allocated
SET catalog_branch = 2000000000;
CREATE SCHEMA app VERSIONED;

-- a shared schema beside the versioned one
RESET catalog_branch;
CREATE SCHEMA pub;
CREATE TABLE pub.p (id int PRIMARY KEY, note text);
INSERT INTO pub.p VALUES (1, 'shared');

-- the same versioned schema on two branches, each with its own table app.t
SET catalog_branch = :b1;
CREATE SCHEMA app VERSIONED
	CREATE TABLE t (id int PRIMARY KEY, v int);
INSERT INTO app.t VALUES (1, 10), (2, 20);
SELECT 'app.t'::regclass::oid AS t1 \gset

SET catalog_branch = :b2;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v int, extra text);
INSERT INTO app.t VALUES (1, 100, 'two');
SELECT 'app.t'::regclass::oid AS t2 \gset

-- each branch's rows are its own, under object ids of its own
SELECT :t1 <> :t2 AS own_object_ids;
SELECT * FROM app.t;
SET catalog_branch = :b1;
SELECT * FROM app.t;
SELECT 'app.t'::regclass::oid = :t1 AS resolves_to_b1;

-- both see the shared schema, and each other's writes to it
SET catalog_branch = :b2;
INSERT INTO pub.p VALUES (2, 'from b2');
SET catalog_branch = :b1;
SELECT * FROM pub.p ORDER BY id;

-- a session on no branch cannot name a versioned schema
RESET catalog_branch;
SELECT * FROM app.t;
SELECT to_regnamespace('app') IS NULL AS no_app;

-- one row per branch, with its branch in nspbranch (a session sees only
-- its branch's rows, so this looks with the catalog filter off)
SET catalog_branch_filter = off;
SELECT count(*) AS rows, count(*) FILTER (WHERE nspbranch = :b1) AS on_b1,
	   count(*) FILTER (WHERE nspbranch = :b2) AS on_b2
FROM pg_namespace WHERE nspname = 'app';
RESET catalog_branch_filter;
SELECT nspbranch FROM pg_namespace WHERE nspname = 'pub';

-- a name is shared or versioned, never both
CREATE SCHEMA app;
SET catalog_branch = :b1;
CREATE SCHEMA pub VERSIONED;
CREATE SCHEMA app VERSIONED;
CREATE SCHEMA IF NOT EXISTS app VERSIONED;
ALTER SCHEMA pub RENAME TO app;
CREATE SCHEMA other VERSIONED;
ALTER SCHEMA other RENAME TO pub;
RESET catalog_branch;
CREATE SCHEMA other;

-- renaming a versioned schema on one branch leaves the other's name
SET catalog_branch = :b2;
ALTER SCHEMA app RENAME TO app2;
SELECT count(*) FROM app2.t;
SET catalog_branch = :b1;
SELECT count(*) FROM app.t;
SELECT to_regnamespace('app2') IS NULL AS no_app2_on_b1;
SET catalog_branch = :b2;
ALTER SCHEMA app2 RENAME TO app;

-- ownership changes on one branch only
CREATE ROLE regress_branch_owner;
ALTER SCHEMA app OWNER TO regress_branch_owner;
SELECT nspowner = 'regress_branch_owner'::regrole AS b2_owner
FROM pg_namespace WHERE oid = 'app'::regnamespace;
SET catalog_branch = :b1;
SELECT nspowner = 'regress_branch_owner'::regrole AS b1_owner
FROM pg_namespace WHERE oid = 'app'::regnamespace;

-- a branch change inside a session: names, the search path, prepared
-- statements and SQL functions resolve again
SET search_path = app, public;
PREPARE q AS SELECT count(*) FROM t;
EXECUTE q;
SET catalog_branch = :b2;
EXECUTE q;
SELECT count(*) FROM t;
CREATE FUNCTION pub.count_t() RETURNS bigint LANGUAGE sql
	AS 'SELECT count(*) FROM app.t';
SELECT pub.count_t();
SET catalog_branch = :b1;
SELECT pub.count_t();
RESET search_path;
DEALLOCATE q;

-- inside a transaction, and back when it rolls back
BEGIN;
SET LOCAL catalog_branch = :b2;
SELECT count(*) FROM app.t;
ROLLBACK;
SELECT count(*) FROM app.t;

-- a sequence's cached values do not outlive a branch change
CREATE SEQUENCE app.s;
SELECT nextval('app.s');
SET catalog_branch = :b2;
SELECT currval('app.s');
SELECT lastval();
SET catalog_branch = :b1;
SELECT currval('app.s');

-- parallel workers resolve names on the leader's branch
SET debug_parallel_query = on;
SELECT to_regclass('app.t')::oid = :t1 AS worker_on_b1;
SET catalog_branch = :b2;
SELECT to_regclass('app.t')::oid = :t2 AS worker_on_b2;
RESET debug_parallel_query;

-- temporary tables on a branch, one with a versioned table's name
CREATE TEMP TABLE t (x int);
INSERT INTO t VALUES (1), (2), (3);
SELECT count(*) FROM pg_temp.t;
SELECT count(*) FROM app.t;

-- dropping one branch's schema leaves the other's
DROP SCHEMA app CASCADE;
SELECT * FROM app.t;
SET catalog_branch = :b1;
SELECT count(*) FROM app.t;

-- a foreign key from a versioned table into a shared one
CREATE TABLE app.c (id int PRIMARY KEY, p int REFERENCES pub.p);
INSERT INTO app.c VALUES (1, 1);
INSERT INTO app.c VALUES (2, 99);

DROP SCHEMA app CASCADE;
DROP SCHEMA other;
RESET catalog_branch;
DROP SCHEMA pub CASCADE;
DROP ROLE regress_branch_owner;
