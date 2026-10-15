-- Branch-aware catalogs: a versioned object's catalog rows carry the
-- branch they were made on, and a session sees only shared rows and its own
-- branch's.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

SELECT oid AS main FROM pg_branch WHERE brname = 'main' \gset
SELECT test_branch_current() = :main AS on_main;

CREATE SCHEMA plain;
CREATE TABLE plain.base (id int PRIMARY KEY);
CREATE SCHEMA app VERSIONED;
CREATE TYPE app.mood AS ENUM ('sad', 'ok');
CREATE TABLE app.t (id serial PRIMARY KEY, m app.mood DEFAULT 'ok',
                    CHECK (id > 0));
CREATE TABLE app.child (id int REFERENCES app.t);
CREATE INDEX t_m ON app.t (m);
CREATE VIEW app.v AS SELECT id FROM app.t;
CREATE FUNCTION app.f(int) RETURNS int LANGUAGE sql AS 'SELECT $1';
COMMENT ON TABLE app.t IS 'versioned';
COMMENT ON TABLE plain.base IS 'shared';
ALTER DEFAULT PRIVILEGES IN SCHEMA app GRANT SELECT ON TABLES TO PUBLIC;
ALTER DEFAULT PRIVILEGES IN SCHEMA plain GRANT SELECT ON TABLES TO PUBLIC;

-- Rows of versioned objects are on main; shared objects' rows are on no branch.
SELECT nspname, nspbranch = :main AS on_main FROM pg_namespace
  WHERE nspname IN ('app', 'plain') ORDER BY 1;
SELECT relname, relbranch = :main AS on_main FROM pg_class
  WHERE relnamespace IN ('app'::regnamespace, 'plain'::regnamespace)
  ORDER BY 1;
SELECT DISTINCT attrelid::regclass, attbranch = :main AS on_main
  FROM pg_attribute
  WHERE attrelid IN ('app.t'::regclass, 'app.t_m'::regclass, 'plain.base'::regclass)
  ORDER BY 1;
SELECT indexrelid::regclass, indbranch = :main AS on_main FROM pg_index
  WHERE indrelid IN ('app.t'::regclass, 'plain.base'::regclass) ORDER BY 1;
SELECT conname, conbranch = :main AS on_main FROM pg_constraint
  WHERE connamespace IN ('app'::regnamespace, 'plain'::regnamespace) ORDER BY 1;
SELECT DISTINCT tgrelid::regclass, tgbranch = :main AS on_main FROM pg_trigger
  WHERE tgrelid IN ('app.t'::regclass, 'app.child'::regclass) ORDER BY 1;
SELECT adrelid::regclass, adbranch = :main AS on_main FROM pg_attrdef
  WHERE adrelid = 'app.t'::regclass ORDER BY adnum;
SELECT ev_class::regclass, ev_branch = :main AS on_main FROM pg_rewrite
  WHERE ev_class = 'app.v'::regclass;
SELECT typname, typbranch = :main AS on_main FROM pg_type
  WHERE typnamespace IN ('app'::regnamespace, 'plain'::regnamespace) ORDER BY 1;
SELECT enumlabel, enumbranch = :main AS on_main FROM pg_enum
  WHERE enumtypid = 'app.mood'::regtype ORDER BY enumsortorder;
SELECT proname, probranch = :main AS on_main FROM pg_proc
  WHERE pronamespace = 'app'::regnamespace;
SELECT seqrelid::regclass, seqbranch = :main AS on_main FROM pg_sequence
  WHERE seqrelid = 'app.t_id_seq'::regclass;
SELECT objoid::regclass, objbranch = :main AS on_main FROM pg_description
  WHERE objoid IN ('app.t'::regclass, 'plain.base'::regclass) ORDER BY 1;
SELECT defaclnamespace::regnamespace, defaclbranch = :main AS on_main
  FROM pg_default_acl
  WHERE defaclnamespace IN ('app'::regnamespace, 'plain'::regnamespace) ORDER BY 1;
SELECT DISTINCT classid::regclass, depbranch = :main AS on_main FROM pg_depend
  WHERE objid IN (SELECT oid FROM pg_class WHERE relnamespace = 'app'::regnamespace
                  UNION SELECT oid FROM pg_type WHERE typnamespace = 'app'::regnamespace
                  UNION SELECT oid FROM pg_constraint WHERE connamespace = 'app'::regnamespace)
  ORDER BY 1, 2;
SELECT count(*) AS shared_deps_on_main FROM pg_depend
  WHERE objid = 'plain.base'::regclass AND depbranch <> 0;

INSERT INTO app.t (m) VALUES ('sad'), (DEFAULT);

-- Another branch (made by hand: branches can't be made yet) sees none of
-- main's versioned objects, and can make its own with the same names.
SET allow_system_table_mods = on;
INSERT INTO pg_branch VALUES (99999, 'other');
RESET allow_system_table_mods;
SELECT test_branch_switch('other');
SELECT test_branch_current() = 99999 AS on_other;
SELECT to_regnamespace('app') IS NULL AS no_app, to_regclass('app.t') IS NULL AS no_t;
SELECT count(*) AS main_rows_seen FROM pg_class WHERE relbranch = :main;
SELECT count(*) FROM plain.base;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (name text);
INSERT INTO app.t VALUES ('other');
SELECT * FROM app.t;
SELECT relname, relbranch = 99999 AS on_other FROM pg_class
  WHERE relnamespace = 'app'::regnamespace ORDER BY 1;
SELECT 'app.t'::regclass::oid AS other_t \gset

-- Parallel workers see the leader's branch.
CREATE FUNCTION app.f(int) RETURNS int LANGUAGE plpgsql PARALLEL SAFE
  AS 'BEGIN RETURN $1 + 1; END';
SET debug_parallel_query = on;
SELECT app.f(1);
SELECT name, app.f(length(name)) FROM app.t;
RESET debug_parallel_query;

-- Rewriting an index keeps its pg_class row on the branch: a TOAST table's
-- index, which is in pg_toast, too.
SELECT i.indexrelid AS other_toast_index FROM pg_index i
  WHERE i.indrelid = (SELECT reltoastrelid FROM pg_class WHERE oid = 'app.t'::regclass) \gset
REINDEX TABLE app.t;
SELECT relbranch = 99999 AS on_other FROM pg_class WHERE oid = :other_toast_index;

-- Back on main, main's objects are back and other's can't be seen, even by
-- an index-only scan over all-visible pages.
SELECT test_branch_switch('main');
SELECT id, m FROM app.t ORDER BY id;
SELECT attname FROM pg_attribute WHERE attrelid = 'app.t'::regclass AND attnum > 0
  ORDER BY attnum;
VACUUM pg_class;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
CREATE FUNCTION pg_temp.plan_of(q text) RETURNS SETOF text LANGUAGE plpgsql AS
$$ DECLARE l text; BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    IF l LIKE '%Scan%' THEN RETURN NEXT l; END IF;
  END LOOP;
END $$;
SELECT pg_temp.plan_of('SELECT oid FROM pg_class WHERE oid = ' || :other_t);
SELECT count(*) AS other_t_seen FROM pg_class WHERE oid = :other_t;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT count(*) AS other_t_seen FROM pg_class WHERE oid = :other_t;
SELECT count(*) AS other_rows_seen FROM pg_class WHERE relbranch = 99999;
SELECT count(*) AS other_toast_index_seen FROM pg_class WHERE oid = :other_toast_index;

-- Each branch drops its own.
DROP SCHEMA app CASCADE;
SELECT test_branch_switch('other');
DROP SCHEMA app CASCADE;
SELECT test_branch_switch('main');
DROP SCHEMA plain CASCADE;
SET allow_system_table_mods = on;
DELETE FROM pg_branch WHERE brname = 'other';
RESET allow_system_table_mods;
