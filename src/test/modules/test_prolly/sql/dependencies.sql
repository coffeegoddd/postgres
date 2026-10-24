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
CREATE FUNCTION dep_util.U&"tab\0009in"() RETURNS int	-- a tab in its name
  LANGUAGE sql AS 'SELECT 7';
CREATE FUNCTION dep_util.twice(int) RETURNS int LANGUAGE sql AS 'SELECT $1 * 2';

CREATE SCHEMA dep VERSIONED;
CREATE TABLE dep.plain (id int PRIMARY KEY, n numeric, t text);
SELECT length(dolt_commit('-Am', 'plain')::text) AS committed;
SELECT test_commit_dependencies(dolt_hashof('HEAD'));	-- none: built-ins only

CREATE TABLE dep.t (id int PRIMARY KEY, m dep_util.mood,
					c citext DEFAULT dep_util.twice(1)::text);
CREATE FUNCTION dep.seven() RETURNS int LANGUAGE sql
  BEGIN ATOMIC SELECT dep_util.U&"tab\0009in"(); END;
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

-- A clone checks the dependencies every branch's head recorded before it
-- makes anything, naming all that is missing; then makes the extensions
-- the database lacks, at the newest version a branch recorded.
SELECT current_database() AS db \gset
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/deps-' || txid_current() AS url \gset
CREATE EXTENSION ltree VERSION '1.2';
CREATE TABLE dep.c (id int PRIMARY KEY, p ltree);
INSERT INTO dep.c VALUES (1, 'a.b');
INSERT INTO dep.t (id, m) VALUES (1, 'ok');
SELECT length(dolt_commit('-Am', 'ltree at 1.2')::text) AS committed;
SELECT dolt_checkout('dep_side');
ALTER EXTENSION ltree UPDATE TO '1.3';
CREATE TABLE dep.c2 (id int PRIMARY KEY, p ltree);
SELECT length(dolt_commit('-Am', 'ltree at 1.3')::text) AS committed;
SELECT dolt_checkout('main');
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_push('origin', 'main'), dolt_push('origin', 'dep_side');

CREATE DATABASE regress_deps_a;
\c regress_deps_a
SELECT dolt_clone(:'url');					-- refused: the types and function
SELECT count(*) AS extensions FROM pg_extension WHERE extname <> 'plpgsql';
SELECT count(*) AS branches FROM dolt_branches WHERE hash <> '';
CREATE SCHEMA dep_util;
CREATE TYPE dep_util.mood AS ENUM ('ok', 'meh');
CREATE FUNCTION dep_util.U&"tab\0009in"() RETURNS int LANGUAGE sql AS 'SELECT 7';
CREATE FUNCTION dep_util.twice(int) RETURNS int LANGUAGE sql AS 'SELECT $1 * 2';
SELECT dolt_clone(:'url');
SELECT extname, extversion FROM pg_extension
  WHERE extname <> 'plpgsql' ORDER BY extname;	-- the newest recorded
SELECT id, p, dep.seven() FROM dep.c;
\c :db
DROP DATABASE regress_deps_a;

-- an extension the database has at an older version: refused, never changed
CREATE DATABASE regress_deps_b;
\c regress_deps_b
CREATE EXTENSION ltree VERSION '1.1';
CREATE SCHEMA dep_util;
CREATE TYPE dep_util.mood AS ENUM ('ok', 'meh');
CREATE FUNCTION dep_util.U&"tab\0009in"() RETURNS int LANGUAGE sql AS 'SELECT 7';
CREATE FUNCTION dep_util.twice(int) RETURNS int LANGUAGE sql AS 'SELECT $1 * 2';
SELECT dolt_clone(:'url');
SELECT extversion FROM pg_extension WHERE extname = 'ltree';
-- at the version needed: used as it is
ALTER EXTENSION ltree UPDATE TO '1.3';
SELECT dolt_clone(:'url');
SELECT count(*) FROM dep.c;
\c :db
DROP DATABASE regress_deps_b;

-- What commits made elsewhere may record, checked directly (a commit
-- recording given dependencies, checked as a clone checks its heads).
CREATE DATABASE regress_deps_c;
\c regress_deps_c
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
SELECT test_fold_root() AS root \gset
CREATE FUNCTION pg_temp.ext(text) RETURNS text LANGUAGE sql
  AS $$ SELECT 'extension ' || $1 $$;

-- an extension that requires another recorded one is made after it, at the
-- recorded version (test_prolly_a sorts first, and requires test_prolly_b,
-- whose default is 1.1)
SELECT test_commit_needing(:'root', ARRAY[pg_temp.ext('test_prolly_a'), pg_temp.ext('test_prolly_b')],
						   ARRAY['{1.0,public}', '{1.0,public}']) AS c \gset
SELECT test_check_dependencies(ARRAY[:'c'], true);
SELECT extname, extversion FROM pg_extension
  WHERE extname LIKE 'test_prolly_%' ORDER BY extname;

-- an extension the server has no control file for, at two versions: named
-- with the rest, not an error of its own
SELECT test_commit_needing(:'root', ARRAY[pg_temp.ext('no_such_ext'), 'type dep_util.mood'],
						   ARRAY['{1.0,public}', '{type,"{dep_util.mood}","{}"}']) AS c1 \gset
SELECT test_commit_needing(:'root', ARRAY[pg_temp.ext('no_such_ext')],
						   ARRAY['{1.1,public}']) AS c2 \gset
SELECT test_check_dependencies(ARRAY[:'c1', :'c2'], true);

-- an object whose address names a type of an extension to be made: the
-- extension isn't missing (it would be made), the object is
SELECT test_commit_needing(:'root', ARRAY[pg_temp.ext('ltree'), 'function dep_util.lf(public.ltree)'],
						   ARRAY['{1.3,public}', '{function,"{dep_util,lf}","{public.ltree}"}']) AS c \gset
SELECT test_check_dependencies(ARRAY[:'c'], true);
SELECT count(*) AS ltree FROM pg_extension WHERE extname = 'ltree';	-- (none made)

-- an extension recorded without a version: any; made at its default
SELECT test_commit_needing(:'root', ARRAY[pg_temp.ext('ltree')], ARRAY['{"",public}']) AS c \gset
SELECT test_check_dependencies(ARRAY[:'c'], true);
SELECT extversion FROM pg_extension WHERE extname = 'ltree';
CREATE EXTENSION hstore VERSION '1.4';
SELECT test_commit_needing(:'root', ARRAY[pg_temp.ext('hstore')], ARRAY['{"",public}']) AS c \gset
SELECT test_check_dependencies(ARRAY[:'c'], false);	-- what it has will do
SELECT extversion FROM pg_extension WHERE extname = 'hstore';

-- an extension's schema: made there, which must exist; one the database
-- has elsewhere, or that commits record in different schemas, refused
SELECT test_commit_needing(:'root', ARRAY[pg_temp.ext('citext')], ARRAY['{1.8,dep_ext}']) AS c \gset
SELECT test_check_dependencies(ARRAY[:'c'], true);
CREATE SCHEMA dep_ext;
SELECT test_check_dependencies(ARRAY[:'c'], true);
SELECT extnamespace::regnamespace FROM pg_extension WHERE extname = 'citext';
SELECT test_commit_needing(:'root', ARRAY[pg_temp.ext('citext')], ARRAY['{1.8,public}']) AS c2 \gset
SELECT test_check_dependencies(ARRAY[:'c2'], false);
SELECT test_check_dependencies(ARRAY[:'c', :'c2'], false);
-- the remedy for an extension in another schema: moved where it can be
-- (citext, above), made again where it can't (test_prolly_ext_table);
-- several at once; and joined with another kind of problem's, "then try
-- again" once
CREATE EXTENSION test_prolly_ext_table SCHEMA dep_ext;
SELECT test_commit_needing(:'root', ARRAY[pg_temp.ext('test_prolly_ext_table')], ARRAY['{1.0,public}']) AS c3 \gset
SELECT test_check_dependencies(ARRAY[:'c3'], false);
SELECT test_commit_needing(:'root', ARRAY[pg_temp.ext('citext'), pg_temp.ext('hstore')],
						   ARRAY['{"",public}', '{"",dep_ext}']) AS c4 \gset
SELECT test_check_dependencies(ARRAY[:'c4'], false);
SELECT test_commit_needing(:'root', ARRAY[pg_temp.ext('citext'), pg_temp.ext('cube')],
						   ARRAY['{"",public}', '{"",public}']) AS c5 \gset
SELECT test_check_dependencies(ARRAY[:'c5'], false);
DROP EXTENSION test_prolly_ext_table;

-- an error other than "not found" while looking for an object is raised
-- as it is, not taken for a missing object
SELECT test_commit_needing(:'root', ARRAY['nonsense x'], ARRAY['{nonsense,"{x}","{}"}']) AS c \gset
SELECT test_check_dependencies(ARRAY[:'c'], false);
\c :db
DROP DATABASE regress_deps_c;

DROP SCHEMA dep CASCADE;
SELECT length(dolt_commit('-Am', 'dep gone')::text) AS committed;
SELECT dolt_branch('-D', 'dep_side');
DROP SCHEMA dep_util CASCADE;
DROP EXTENSION citext;
DROP EXTENSION hstore;
DROP EXTENSION ltree;
DROP ROLE regress_dep_reader;
