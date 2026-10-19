-- dolt_clone: an empty database made a copy of a remote's history, every
-- branch a branch here (head-only until used), the default one checked
-- out.  A clone whose objects need something the database lacks (here, a
-- type of an ordinary schema) leaves nothing.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
-- (this build's canonical format, for a remote's head set by hand)
SELECT value AS fmt FROM test_fold_items() WHERE key = '!:format' \gset

SELECT current_database() AS db \gset
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/clone-' || txid_current() AS run \gset
\set url :run '/a'
\set url_dep :run '/dep'
\set url_nomain :run '/nomain'
\set url_odd :run '/odd'

CREATE SCHEMA cl VERSIONED;
CREATE TABLE cl.t (id int PRIMARY KEY, v text);
INSERT INTO cl.t SELECT g, 'v' || g FROM generate_series(1, 200) g;
SELECT length(dolt_commit('-Am', 'cl 1')::text) AS committed;
SELECT dolt_branch('side');
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_push('origin', 'main') AS pushed;
SELECT dolt_checkout('side') IS NOT NULL AS on_side;
UPDATE cl.t SET v = 'side' WHERE id = 1;
SELECT length(dolt_commit('-am', 'on side')::text) AS committed;
SELECT dolt_push('origin', 'side') AS pushed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;

-- a clone: main checked out at the remote's, side a branch at its
CREATE DATABASE regress_clone_b;
\c regress_clone_b
SELECT dolt_clone(:'url');
SELECT count(*), max(v) FROM cl.t;
SELECT name FROM dolt_branches ORDER BY name;
SELECT name FROM dolt_remote_branches ORDER BY name;
SELECT name, url = :'url' AS url_given FROM dolt_remotes;
SELECT dolt_pull() IS NOT NULL AS pulled_from_upstream;
SELECT dolt_checkout('side') IS NOT NULL AS on_side;
SELECT v FROM cl.t WHERE id = 1;

-- a database with commits can't take a clone
SELECT dolt_clone(:'url');
\c :db
DROP DATABASE regress_clone_b;

-- what a clone would run into, named; a remote named as dolt_remote names
-- one
CREATE DATABASE regress_clone_e;
\c regress_clone_e
SELECT dolt_clone(:'url', '--remote', 'a/b');
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_branch('side');
SELECT dolt_clone(:'url');
SELECT dolt_remote('remove', 'origin');
SELECT dolt_clone(:'url');
\c :db
DROP DATABASE regress_clone_e;

-- a remote without main: its master checked out; main, the database's,
-- stays, empty
SELECT dolt_remote('add', 'nomain', :'url_nomain');
SELECT dolt_push('nomain', 'side') AS pushed;
SELECT dolt_branch('master', 'side');
SELECT dolt_push('nomain', 'master') AS pushed;
SELECT dolt_branch('-D', 'master');
CREATE DATABASE regress_clone_f;
\c regress_clone_f
SELECT dolt_clone(:'url_nomain');
SELECT active_branch();
SELECT name, hash IS NULL AS empty FROM dolt_branches ORDER BY name;
\c :db
DROP DATABASE regress_clone_f;
SELECT dolt_remote('remove', 'nomain');

-- --branch: another branch checked out
CREATE DATABASE regress_clone_c;
\c regress_clone_c
SELECT dolt_clone(:'url', '--branch', 'nowhere');
SELECT dolt_clone(:'url', '--branch', 'side', '--remote', 'upstream');
SELECT active_branch();
SELECT name FROM dolt_remotes;
\c :db
DROP DATABASE regress_clone_c;

-- a remote's branch whose name isn't one here (with "..", or made under
-- older rules) isn't made a branch, with a warning: its commits are
-- fetched, its tracking branch there to make one of it
SELECT dolt_remote('add', 'oddname', :'url_odd');
SELECT dolt_push('oddname', 'main') AS pushed;
SELECT test_remote_set_head(:'url_odd', 'odd..name', dolt_hashof('main'), :'fmt');
CREATE DATABASE regress_clone_g;
\c regress_clone_g
SELECT dolt_clone(:'url_odd');
SELECT name FROM dolt_branches ORDER BY 1;
SELECT dolt_branch('odd_name', 'origin/odd..name');
SELECT dolt_hashof('odd_name') = dolt_hashof('main') AS tracked;
\c :db
DROP DATABASE regress_clone_g;
SELECT dolt_remote('remove', 'oddname');

-- a commit needing a type of an ordinary schema the clone lacks
CREATE TYPE public.cl_mood AS ENUM ('sad', 'ok');
CREATE TABLE cl.m (id int PRIMARY KEY, mood public.cl_mood);
INSERT INTO cl.m VALUES (1, 'ok');
SELECT length(dolt_commit('-Am', 'moods')::text) AS committed;
SELECT dolt_remote('add', 'dep', :'url_dep');
SELECT dolt_push('dep', 'main') AS pushed;
CREATE DATABASE regress_clone_d;
\c regress_clone_d
SELECT dolt_clone(:'url_dep');
SELECT count(*) AS remotes FROM dolt_remotes;
SELECT count(*) AS branches_with_commits FROM dolt_branches WHERE hash <> '';
CREATE TYPE public.cl_mood AS ENUM ('sad', 'ok');
SELECT dolt_clone(:'url_dep');
SELECT mood FROM cl.m;
\c :db
DROP DATABASE regress_clone_d;

SELECT dolt_remote('remove', 'origin'), dolt_remote('remove', 'dep');
SELECT dolt_branch('-D', 'side');
DROP SCHEMA cl CASCADE;
SELECT length(dolt_commit('-Am', 'cl gone')::text) AS committed;
DROP TYPE public.cl_mood;

-- a history whose public is versioned, cloned into a new database: its
-- public, the default one, unversioned and empty, is replaced; one with
-- objects in it is refused, named, and the clone leaves nothing
\set url_pub :run '/pub'
CREATE DATABASE regress_clone_p;
\c regress_clone_p
SET default_table_access_method = prolly;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public VERSIONED;
CREATE TABLE t (id int PRIMARY KEY);
INSERT INTO t VALUES (1);
SELECT length(dolt_commit('-Am', 'p 1')::text) AS committed;
SELECT dolt_remote('add', 'origin', :'url_pub');
SELECT dolt_push('origin', 'main') AS pushed;
SELECT dolt_branch('side');
SELECT dolt_push('origin', 'side') AS pushed;
CREATE DATABASE regress_clone_q;
\c regress_clone_q
SELECT dolt_clone(:'url_pub');
SELECT id FROM t;
SELECT nspversioned FROM pg_namespace WHERE nspname = 'public';
-- (what a commit doesn't hold kept: a new database's public, as it was)
SELECT nspowner::regrole AS owner, nspacl,
       obj_description(oid, 'pg_namespace') AS comment
FROM pg_namespace WHERE nspname = 'public';
SELECT privtype, initprivs FROM pg_init_privs
WHERE objoid = 'public'::regnamespace AND classoid = 'pg_namespace'::regclass;
SELECT count(*) AS uncommitted FROM dolt_status;
-- (another branch, its versioned public the same)
SELECT dolt_checkout('side') IS NOT NULL AS on_side;
SELECT id FROM t;
SELECT nspversioned, nspowner::regrole AS owner, nspacl
FROM pg_namespace WHERE nspname = 'public';
\c regress_clone_p
DROP DATABASE regress_clone_q;
CREATE DATABASE regress_clone_r;
\c regress_clone_r
CREATE TABLE public.mine (id int);
SELECT dolt_clone(:'url_pub');
SELECT count(*) AS remotes FROM dolt_remotes;
SELECT nspversioned, (SELECT count(*) FROM pg_class WHERE relnamespace = n.oid) AS objects
FROM pg_namespace n WHERE nspname = 'public';
DROP TABLE public.mine;
SELECT dolt_clone(:'url_pub');
SELECT id FROM t;
\c :db
DROP DATABASE regress_clone_r;
DROP DATABASE regress_clone_p;

-- a public given to others, its owner, grants and comment kept on the
-- versioned one; one made by another role stays that role's
CREATE ROLE regress_clone_dbo;
CREATE ROLE regress_clone_app;
CREATE DATABASE regress_clone_g OWNER regress_clone_dbo;
\c regress_clone_g
GRANT USAGE, CREATE ON SCHEMA public TO regress_clone_app;
COMMENT ON SCHEMA public IS 'granted';
LOAD 'dummy_seclabel';
SECURITY LABEL ON SCHEMA public IS 'classified';
SELECT dolt_clone(:'url_pub');
SELECT provider, label FROM pg_seclabels
WHERE objoid = 'public'::regnamespace AND classoid = 'pg_namespace'::regclass;
SELECT nspversioned, nspowner::regrole AS owner, nspacl,
       obj_description(oid, 'pg_namespace') AS comment
FROM pg_namespace WHERE nspname = 'public';
SELECT has_schema_privilege('regress_clone_app', 'public', 'USAGE, CREATE') AS app,
       has_schema_privilege('regress_clone_dbo', 'public', 'CREATE') AS dbo,
       has_schema_privilege('public', 'public', 'USAGE') AS everyone;
SELECT count(*) AS grant_dependencies FROM pg_shdepend
WHERE objid = 'public'::regnamespace AND refobjid = 'regress_clone_app'::regrole;
SELECT count(*) AS uncommitted FROM dolt_status;
\c :db
DROP DATABASE regress_clone_g;
CREATE DATABASE regress_clone_a;
\c regress_clone_a
DROP SCHEMA public;
CREATE SCHEMA public AUTHORIZATION regress_clone_app;
SELECT dolt_clone(:'url_pub');
SELECT nspversioned, nspowner::regrole AS owner, nspacl
FROM pg_namespace WHERE nspname = 'public';
-- default privileges for it are something depending on it: refused
\c :db
DROP DATABASE regress_clone_a;
CREATE DATABASE regress_clone_d;
\c regress_clone_d
ALTER DEFAULT PRIVILEGES FOR ROLE regress_clone_dbo IN SCHEMA public GRANT SELECT ON TABLES TO regress_clone_app;
SELECT dolt_clone(:'url_pub');
\c :db
DROP DATABASE regress_clone_d;

-- a later move, here a hard reset of main to a history with a versioned
-- public: main's own commits use the ordinary public (and dev's history
-- has none), refused, main and dev keeping it
CREATE DATABASE regress_clone_m;
\c regress_clone_m
GRANT USAGE ON SCHEMA public TO regress_clone_app;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.a (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'app')::text) AS committed;
SELECT dolt_branch('dev');
DROP SCHEMA app CASCADE;
SELECT length(dolt_commit('-Am', 'app gone')::text) AS committed;
SELECT dolt_remote('add', 'origin', :'url_pub');
SELECT dolt_fetch('origin') IS NOT NULL AS fetched;
SELECT dolt_reset('--hard', 'origin/main');
-- (refused for main's commits even where every other branch has one)
SELECT dolt_branch('-D', 'dev');
SELECT dolt_branch('b2', 'origin/main');
SELECT dolt_reset('--hard', 'origin/main');
SELECT dolt_branch('dev', 'HEAD~1');
SELECT nspversioned, nspacl FROM pg_namespace WHERE nspname = 'public';
SELECT dolt_checkout('dev') IS NOT NULL AS on_dev;
CREATE TABLE x (id int);
SELECT nspversioned FROM pg_namespace n JOIN pg_class c ON c.relnamespace = n.oid
WHERE c.relname = 'x';
\c :db
DROP DATABASE regress_clone_m;

-- a clone of a history one of whose branches has no versioned public: that
-- branch uses the ordinary one, refused, naming it
\set url_mix :run '/mix'
CREATE DATABASE regress_clone_x;
\c regress_clone_x
SET default_table_access_method = prolly;
CREATE SCHEMA s VERSIONED;
CREATE TABLE s.a (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'bare')::text) AS committed;
SELECT dolt_branch('bare');
DROP SCHEMA public;
CREATE SCHEMA public VERSIONED;
SELECT length(dolt_commit('-Am', 'pub')::text) AS committed;
SELECT dolt_remote('add', 'origin', :'url_mix');
SELECT dolt_push('origin', 'main') AS pushed;
SELECT dolt_push('origin', 'bare') AS pushed;
CREATE DATABASE regress_clone_y;
\c regress_clone_y
SELECT dolt_clone(:'url_mix');
SELECT nspversioned FROM pg_namespace WHERE nspname = 'public';
\c :db
DROP DATABASE regress_clone_y;
DROP DATABASE regress_clone_x;
DROP ROLE regress_clone_app;
DROP ROLE regress_clone_dbo;
