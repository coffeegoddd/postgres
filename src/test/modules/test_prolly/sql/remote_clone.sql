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
