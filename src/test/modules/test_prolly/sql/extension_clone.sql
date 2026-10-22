-- A commit records each extension it needs with its version and schema,
-- and whether that schema is versioned.  A clone makes such a schema,
-- versioned, before it makes the extension in it: in place of a new
-- database's ordinary public (as a clone's first history replaces it), or
-- afresh.

SELECT current_database() AS db \gset
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/extclone-' || txid_current() AS url \gset

CREATE DATABASE regress_extclone_src;
\c regress_extclone_src
CREATE SCHEMA tools;
CREATE EXTENSION test_prolly SCHEMA tools;
DROP SCHEMA public;
CREATE SCHEMA public VERSIONED;
CREATE SCHEMA geo VERSIONED;
CREATE EXTENSION citext SCHEMA public;
CREATE EXTENSION hstore SCHEMA geo;
CREATE TABLE public.t (id int PRIMARY KEY, name public.citext);
CREATE TABLE geo.g (id int PRIMARY KEY, attrs geo.hstore);
INSERT INTO public.t VALUES (1, 'Hi');
INSERT INTO geo.g VALUES (1, 'a=>1');
SELECT length(dolt_commit('-Am', 'extensions in versioned schemas')::text) AS committed;
SELECT replace(d, E'\t', ' | ') AS dependency
  FROM unnest(tools.test_commit_dependencies(dolt_hashof('HEAD'))) d ORDER BY 1;
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_push('origin', 'main');

-- a new database: public ordinary, no geo
\c :db
CREATE DATABASE regress_extclone_dst;
\c regress_extclone_dst
SELECT dolt_clone(:'url');
SELECT nspname, nspversioned FROM pg_namespace
 WHERE nspname IN ('public', 'geo') ORDER BY 1;
SELECT extname, extnamespace::regnamespace AS schema FROM pg_extension
 WHERE extname IN ('citext', 'hstore') ORDER BY 1;
SELECT id, name FROM public.t;
SELECT id, attrs FROM geo.g;
SELECT table_name, status FROM dolt_status() ORDER BY 1;
SELECT name = 'HI' AS citext_compares FROM public.t;

\c :db
DROP DATABASE regress_extclone_dst;
DROP DATABASE regress_extclone_src;
