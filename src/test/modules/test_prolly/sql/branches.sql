-- Branches of versioned schemas: dolt_branch copies the session's branch,
-- dolt_checkout switches to another, and each branch's changes, to rows
-- and to schema, are its own.

CREATE SCHEMA app VERSIONED;
CREATE TYPE app.mood AS ENUM ('sad', 'ok');
CREATE TABLE app.t (id serial PRIMARY KEY, m app.mood, v text);
CREATE INDEX t_m ON app.t (m);
CREATE VIEW app.v AS SELECT count(*) AS n FROM app.t;
CREATE FUNCTION app.f() RETURNS text LANGUAGE sql AS 'SELECT ''main''';
CREATE TABLE shared_t (x int);
INSERT INTO shared_t VALUES (1);
-- (rows long enough to be TOASTed; no checkpoint, so they are in buffers)
INSERT INTO app.t (m, v) SELECT 'ok', repeat(g::text, 1000) FROM generate_series(1, 50) g;
SELECT active_branch();

-- A branch starts as a copy of the session's branch.
SELECT dolt_branch('feature');
SELECT name FROM dolt_branches() ORDER BY name;
SELECT dolt_checkout('feature');
SELECT active_branch();
SELECT count(*), sum(length(v)) FROM app.t;
SELECT n FROM app.v;
SELECT app.f();
SELECT count(*) FROM app.t WHERE m = 'ok';
SET enable_seqscan = off;
SELECT count(*) FROM app.t WHERE m = 'ok';
RESET enable_seqscan;

-- Its changes are its own: rows, schema, sequences.
INSERT INTO app.t (m, v) VALUES ('sad', 'feature');
UPDATE app.t SET v = 'changed' WHERE id = 1;
DELETE FROM app.t WHERE id = 2;
ALTER TABLE app.t ADD COLUMN extra int DEFAULT 7;
ALTER TYPE app.mood ADD VALUE 'happy';
CREATE TABLE app.only_feature (x int);
DROP VIEW app.v;
CREATE OR REPLACE FUNCTION app.f() RETURNS text LANGUAGE sql AS 'SELECT ''feature''';
SELECT count(*), count(*) FILTER (WHERE v = 'changed'), max(id) FROM app.t;
SELECT app.f();

SELECT dolt_checkout('main');
SELECT count(*), count(*) FILTER (WHERE v = 'changed'), max(id) FROM app.t;
SELECT string_agg(attname, ',' ORDER BY attnum) FROM pg_attribute
  WHERE attrelid = 'app.t'::regclass AND attnum > 0 AND NOT attisdropped;
SELECT enum_range(NULL::app.mood);
SELECT to_regclass('app.only_feature') IS NULL AS no_feature_table;
SELECT n FROM app.v;
SELECT app.f();
SELECT nextval('app.t_id_seq');

-- Main's changes are its own too.
INSERT INTO app.t (m, v) VALUES ('ok', 'main');
SELECT dolt_checkout('feature');
SELECT count(*) FILTER (WHERE v = 'main') AS main_row_here FROM app.t;
SELECT nextval('app.t_id_seq');

-- Shared objects are shared.
INSERT INTO shared_t VALUES (2);
SELECT dolt_checkout('main');
SELECT count(*) FROM shared_t;

-- A branch of another branch, by name.
SELECT dolt_branch('feature2', 'feature');
SELECT dolt_checkout('feature2');
SELECT count(*), count(*) FILTER (WHERE v = 'changed') FROM app.t;
SELECT to_regclass('app.only_feature') IS NOT NULL AS has_feature_table;
SELECT dolt_checkout('main');

-- dolt_checkout -b makes and checks out.
SELECT dolt_checkout('-b', 'quick');
SELECT active_branch();
SELECT dolt_checkout('main');

-- Making a branch is transactional.
BEGIN;
SELECT dolt_branch('rolled_back');
ROLLBACK;
SELECT name FROM dolt_branches() ORDER BY name;

-- A checkout of a branch whose making rolls back goes back to the branch
-- it left: at a rollback, a rollback to a savepoint, through a chain.
BEGIN;
SELECT dolt_checkout('-b', 'aborted');
SELECT active_branch();
ROLLBACK;
SELECT active_branch();
SELECT count(*) > 0 AS has_rows FROM app.t;
BEGIN;
SAVEPOINT s;
SELECT dolt_checkout('-b', 'aborted_sub');
ROLLBACK TO s;
SELECT active_branch();
SELECT count(*) > 0 AS has_rows FROM app.t;
COMMIT;
BEGIN;
SELECT dolt_checkout('-b', 'aborted1');
SAVEPOINT s;
SELECT dolt_checkout('-b', 'aborted2');
ROLLBACK;
SELECT active_branch();
BEGIN;
SAVEPOINT s;
SELECT dolt_checkout('-b', 'kept');
RELEASE s;
COMMIT;
SELECT active_branch();
SELECT dolt_checkout('main');
SELECT dolt_branch('-d', 'kept');
SELECT name FROM dolt_branches() ORDER BY name;

-- Making a branch takes CREATE on the database; deleting one, a head
-- move's privileges: owning the branch (having made it) is enough.
CREATE ROLE regress_branch_user;
SET ROLE regress_branch_user;
DO $$ BEGIN
	PERFORM dolt_branch('by_user');
EXCEPTION WHEN insufficient_privilege THEN
	RAISE NOTICE 'making a branch was refused';
END $$;
RESET ROLE;
DO $$ BEGIN
	EXECUTE format('GRANT CREATE ON DATABASE %I TO regress_branch_user',
				   current_database());
END $$;
SET ROLE regress_branch_user;
SELECT dolt_branch('by_user');
RESET ROLE;
SELECT dolt_branch('by_owner');
DO $$ BEGIN
	EXECUTE format('REVOKE CREATE ON DATABASE %I FROM regress_branch_user',
				   current_database());
END $$;
SET ROLE regress_branch_user;
\set VERBOSITY terse
SELECT dolt_branch('-d', 'by_owner');
\set VERBOSITY default
SELECT dolt_branch('-d', 'by_user');
RESET ROLE;
SELECT dolt_branch('-d', 'by_owner');
DROP ROLE regress_branch_user;

-- A role a branch's copy references can't be dropped, even once no other
-- branch's copy does; dropping what it owns there, or the branch, frees it.
CREATE ROLE regress_branch_owner;
CREATE TABLE app.owned (x int);
ALTER TABLE app.owned OWNER TO regress_branch_owner;
SELECT dolt_branch('keeps_owner');
SELECT dolt_branch('also_keeps_owner');
DROP TABLE app.owned;
DROP ROLE regress_branch_owner;
SELECT dolt_checkout('keeps_owner');
SELECT pg_get_userbyid(relowner) FROM pg_class WHERE oid = 'app.owned'::regclass;
DROP OWNED BY regress_branch_owner;
SELECT to_regclass('app.owned') IS NULL AS dropped_here;
SELECT dolt_checkout('main');
DROP ROLE regress_branch_owner;
SELECT dolt_branch('-d', 'also_keeps_owner');
DROP ROLE regress_branch_owner;
SELECT dolt_branch('-d', 'keeps_owner');

-- Deleting: not main, not the session's branch.  A branch of a deleted
-- branch keeps the pages it shared.
SELECT dolt_branch('-d', 'main');
SELECT dolt_checkout('feature');
SELECT dolt_branch('-d', 'feature');
SELECT dolt_checkout('main');
SELECT dolt_branch('-d', 'feature');
SELECT dolt_checkout('feature');
SELECT dolt_checkout('feature2');
SELECT count(*), sum(length(v)) FILTER (WHERE v <> 'changed') FROM app.t;
SELECT dolt_checkout('main');
SELECT dolt_branch('-d', 'feature2', 'quick');
SELECT name FROM dolt_branches() ORDER BY name;
SELECT count(*), sum(length(v)) FROM app.t;

DROP SCHEMA app CASCADE;
DROP TABLE shared_t;
