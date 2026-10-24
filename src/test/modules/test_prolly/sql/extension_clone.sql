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

-- the same with the extensions the other way round: whichever schema an
-- extension needs comes first, public is still replaced and geo made
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/extclone2-' || txid_current() AS url2 \gset
CREATE DATABASE regress_extclone_src2;
\c regress_extclone_src2
DROP SCHEMA public;
CREATE SCHEMA public VERSIONED;
CREATE SCHEMA geo VERSIONED;
CREATE EXTENSION hstore SCHEMA public;
CREATE EXTENSION citext SCHEMA geo;
CREATE TABLE public.t (id int PRIMARY KEY, attrs public.hstore);
CREATE TABLE geo.g (id int PRIMARY KEY, name geo.citext);
INSERT INTO public.t VALUES (1, 'a=>1');
INSERT INTO geo.g VALUES (1, 'Hi');
SELECT length(dolt_commit('-Am', 'the other way round')::text) AS committed;
SELECT dolt_remote('add', 'origin', :'url2');
SELECT dolt_push('origin', 'main');
\c :db
CREATE DATABASE regress_extclone_dst2;
\c regress_extclone_dst2
SELECT dolt_clone(:'url2');
SELECT nspname, nspversioned FROM pg_namespace
 WHERE nspname IN ('public', 'geo') ORDER BY 1;
SELECT extname, extnamespace::regnamespace AS schema FROM pg_extension
 WHERE extname IN ('citext', 'hstore') ORDER BY 1;
SELECT id, attrs FROM public.t;
SELECT id, name FROM geo.g;
\c :db
DROP DATABASE regress_extclone_dst2;
DROP DATABASE regress_extclone_src2;

-- an extension whose control file fixes its schema, which isn't versioned:
-- the clone makes it as CREATE EXTENSION does, schema and all
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/extclone3-' || txid_current() AS url3 \gset
CREATE DATABASE regress_extclone_src3;
\c regress_extclone_src3
CREATE EXTENSION test_prolly_fixed;
DROP SCHEMA public;
CREATE SCHEMA public VERSIONED;
CREATE TABLE public.t (id int PRIMARY KEY, n prolly_fixed.posint);
INSERT INTO public.t VALUES (1, 7);
SELECT length(dolt_commit('-Am', 'a fixed schema')::text) AS committed;
SELECT dolt_remote('add', 'origin', :'url3');
SELECT dolt_push('origin', 'main');
\c :db
CREATE DATABASE regress_extclone_dst3;
\c regress_extclone_dst3
SELECT dolt_clone(:'url3');
SELECT extname, extnamespace::regnamespace AS schema FROM pg_extension
 WHERE extname = 'test_prolly_fixed';
SELECT nspname, nspversioned FROM pg_namespace WHERE nspname = 'prolly_fixed';
SELECT id, n FROM public.t;
INSERT INTO public.t VALUES (2, -1);
\c :db
DROP DATABASE regress_extclone_dst3;
DROP DATABASE regress_extclone_src3;

-- a relocatable extension kept in that schema: made after the one whose
-- control file makes the schema
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/extclone4-' || txid_current() AS url4 \gset
CREATE DATABASE regress_extclone_src4;
\c regress_extclone_src4
CREATE EXTENSION test_prolly_fixed;
CREATE EXTENSION test_prolly_b SCHEMA prolly_fixed;
DROP SCHEMA public;
CREATE SCHEMA public VERSIONED;
CREATE TABLE public.t (id int PRIMARY KEY,
	n prolly_fixed.posint DEFAULT prolly_fixed.test_prolly_b_one());
INSERT INTO public.t (id) VALUES (1);
SELECT length(dolt_commit('-Am', 'two in a fixed schema')::text) AS committed;
SELECT dolt_remote('add', 'origin', :'url4');
SELECT dolt_push('origin', 'main');
\c :db
CREATE DATABASE regress_extclone_dst4;
\c regress_extclone_dst4
SELECT dolt_clone(:'url4');
SELECT extname, extnamespace::regnamespace AS schema FROM pg_extension
 WHERE extname IN ('test_prolly_fixed', 'test_prolly_b') ORDER BY 1;
SELECT id, n FROM public.t;
\c :db
DROP DATABASE regress_extclone_dst4;
DROP DATABASE regress_extclone_src4;

-- the commits keep it in a schema its control file here doesn't fix (the
-- schema renamed since it was made): refused, saying so, even where the
-- database has the schema the commits name
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/extclone5-' || txid_current() AS url5 \gset
CREATE DATABASE regress_extclone_src5;
\c regress_extclone_src5
CREATE EXTENSION test_prolly_fixed;
ALTER SCHEMA prolly_fixed RENAME TO fx_a;
DROP SCHEMA public;
CREATE SCHEMA public VERSIONED;
CREATE TABLE public.t (id int PRIMARY KEY, n fx_a.posint);
SELECT length(dolt_commit('-Am', 'a renamed fixed schema')::text) AS committed;
SELECT dolt_remote('add', 'origin', :'url5');
SELECT dolt_push('origin', 'main');
\c :db
CREATE DATABASE regress_extclone_dst5;
\c regress_extclone_dst5
CREATE SCHEMA fx_a;
SELECT dolt_clone(:'url5');
SELECT count(*) AS extensions FROM pg_extension WHERE extname = 'test_prolly_fixed';
\c :db
DROP DATABASE regress_extclone_dst5;
DROP DATABASE regress_extclone_src5;
