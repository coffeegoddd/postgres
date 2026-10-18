-- What versioned objects need from outside the versioned schemas, recorded
-- by each commit beside its root: an extension (with its version) for its
-- members or named by DEPENDS ON EXTENSION, an object of an ordinary schema
-- (with its address).  Not what initdb made (built-in types and functions),
-- and not roles: owners and grants aren't versioned, so a role isn't a
-- dependency.  Not part of the root: what changes outside (an extension's
-- update) never makes a branch's working set differ from its head.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE FUNCTION pg_temp.deps(ref text DEFAULT 'HEAD') RETURNS SETOF text
  LANGUAGE sql AS $$
	SELECT replace(d, E'\t', ' | ')
	  FROM unnest(test_commit_dependencies(dolt_hashof(ref))) d
  $$;

CREATE ROLE regress_dep_reader;
CREATE EXTENSION citext VERSION '1.4';
CREATE EXTENSION hstore;
CREATE SCHEMA dep_util;
CREATE TYPE dep_util.mood AS ENUM ('ok', 'meh');
CREATE TYPE dep_util.U&"tab\0009in" AS ENUM ('x');	-- a tab in its name
CREATE FUNCTION dep_util.twice(int) RETURNS int LANGUAGE sql AS 'SELECT $1 * 2';

CREATE SCHEMA dep VERSIONED;
CREATE TABLE dep.plain (id int PRIMARY KEY, n numeric, t text);
SELECT length(dolt_commit('-Am', 'plain')::text) AS committed;
SELECT test_commit_dependencies(dolt_hashof('HEAD'));	-- none: built-ins only

CREATE TABLE dep.t (id int PRIMARY KEY, m dep_util.mood,
					c citext DEFAULT dep_util.twice(1)::text,
					w dep_util.U&"tab\0009in");
GRANT SELECT ON dep.t TO regress_dep_reader;		-- (not a dependency)
CREATE FUNCTION dep.f() RETURNS int LANGUAGE sql AS 'SELECT 1';
ALTER FUNCTION dep.f() DEPENDS ON EXTENSION hstore;
SELECT length(dolt_commit('-Am', 'deps')::text) AS committed;
SELECT * FROM pg_temp.deps();

-- a commit that didn't record them (as one made before they were): unknown
SELECT test_commit_dependencies(test_commit_of(test_fold_root(), 'older'))
  IS NULL AS unrecorded;

-- an extension updated: no change to the branch; the next commit records it
ALTER EXTENSION citext UPDATE TO '1.6';
SELECT * FROM dolt_status;
SELECT name, dirty FROM dolt_branches ORDER BY name;
SELECT dolt_commit('-Am', 'nothing');
SELECT length(dolt_commit('--allow-empty', '-Am', 'at 1.6')::text) AS committed;
SELECT * FROM pg_temp.deps();

-- a branch whose head recorded another version: clean when checked out
SELECT dolt_branch('dep_side', 'HEAD~1');
SELECT dolt_checkout('dep_side');
SELECT * FROM dolt_status;
CREATE TABLE dep.u (id int PRIMARY KEY, m dep_util.mood);
SELECT length(dolt_commit('-Am', 'u')::text) AS committed;
SELECT * FROM pg_temp.deps();					-- described now: citext 1.6
SELECT dolt_checkout('main');

-- merged over an extension's update, and dependencies that differ: no
-- uncommitted change, no conflict; the merge's commit describes them afresh
ALTER TABLE dep.t ALTER COLUMN c DROP DEFAULT;	-- the function no longer needed
SELECT length(dolt_commit('-Am', 'no default')::text) AS committed;
ALTER EXTENSION citext UPDATE TO '1.8';
SELECT dolt_merge('dep_side') IS NOT NULL AS merged;
SELECT * FROM pg_temp.deps();

DROP SCHEMA dep CASCADE;
SELECT length(dolt_commit('-Am', 'dep gone')::text) AS committed;
SELECT dolt_branch('-D', 'dep_side');
DROP SCHEMA dep_util CASCADE;
DROP EXTENSION citext;
DROP EXTENSION hstore;
DROP ROLE regress_dep_reader;
