-- test_prolly.version_new_schemas (testing only): every schema made while
-- it is on is versioned, however the CREATE SCHEMA runs, and so is the
-- schema CREATE EXTENSION makes for its control file.

LOAD 'test_prolly';
CREATE SCHEMA vns_off;
SET test_prolly.version_new_schemas = on;
CREATE SCHEMA vns_typed;
CREATE SCHEMA vns_already VERSIONED;
CREATE SCHEMA vns_elts CREATE TABLE t (id int);
DO $$ BEGIN EXECUTE 'CREATE SCHEMA vns_executed'; END $$;
CREATE FUNCTION vns_make(name) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE SCHEMA %I', $1);
END $$;
SELECT vns_make('vns_function');
-- (a plpgsql statement's cached plan: the second run sees it too)
CREATE FUNCTION vns_static() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	CREATE SCHEMA vns_static;
END $$;
SELECT vns_static();
DROP SCHEMA vns_static;
SELECT vns_static();
-- (test_prolly_fixed's control file names schema prolly_fixed)
CREATE EXTENSION test_prolly_fixed;
SELECT nspname, nspversioned FROM pg_namespace
  WHERE nspname LIKE 'vns\_%' OR nspname = 'prolly_fixed' ORDER BY 1;
SELECT a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
  WHERE c.oid = 'vns_elts.t'::regclass;
-- the schema isn't the extension's, as without the setting
DROP EXTENSION test_prolly_fixed;
SELECT nspversioned FROM pg_namespace WHERE nspname = 'prolly_fixed';

-- off again: plain schemas
RESET test_prolly.version_new_schemas;
CREATE SCHEMA vns_reset;
SELECT nspversioned FROM pg_namespace WHERE nspname = 'vns_reset';
-- a superuser's setting
CREATE ROLE regress_vns_user;
SET ROLE regress_vns_user;
SET test_prolly.version_new_schemas = on;
RESET ROLE;

DROP FUNCTION vns_make(name), vns_static();
DROP SCHEMA vns_off, vns_typed, vns_already, vns_elts, vns_executed,
  vns_function, vns_static, vns_reset, prolly_fixed CASCADE;
DROP ROLE regress_vns_user;
