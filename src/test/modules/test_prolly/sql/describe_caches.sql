-- What a fold works out for every table, changed or not, cached in the
-- backend (#483): each table's description (describe_cache_hits counts
-- each one taken), the tree of what the versioned objects depend on, and
-- the last canonical tree's items.  Each case changes something a cache
-- holds with the check off (so a cache that missed the change is taken
-- as it is), keeps that session's fold, and compares it with a fresh
-- session's, whose caches are empty.  (This suite runs with
-- debug_prolly_verify_fold_caches on: each one taken is worked out again
-- too, an error if the two differ.)
CREATE DATABASE regress_describe_caches;
\c regress_describe_caches
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
SET dolt.author = 'Ada Lovelace <ada@example.com>';
CREATE SCHEMA dc VERSIONED;
CREATE TYPE dc.mood AS ENUM ('sad', 'ok');
CREATE DOMAIN dc.small AS int;
CREATE FUNCTION dc.touch() RETURNS trigger LANGUAGE plpgsql AS
  $$BEGIN RETURN NEW; END$$;
CREATE TABLE dc.t (id int PRIMARY KEY, v text, m dc.mood, s dc.small);
CREATE TRIGGER t_touch BEFORE UPDATE ON dc.t FOR EACH ROW
  EXECUTE FUNCTION dc.touch();
CREATE TABLE dc.u (id int PRIMARY KEY, w text);
INSERT INTO dc.t VALUES (1, 'a', 'sad', 1);
INSERT INTO dc.u VALUES (1, 'b');
SELECT length(dolt_commit('-Am', 'base')::text) > 0 AS committed;

-- Folds kept by name, in an ordinary (unversioned) schema, for a fresh
-- session to compare with: the working set folded from the baseline (as
-- commits fold, the cached descriptions saying which tables' trees are
-- the baseline's), against one folded in full.
CREATE TABLE public.folds (name text PRIMARY KEY, root text);
CREATE PROCEDURE public.keep(name text) LANGUAGE sql AS
  $$INSERT INTO public.folds VALUES (name, test_fold_root())$$;
CREATE FUNCTION public.same(name text) RETURNS bool LANGUAGE sql AS
  $$SELECT root = test_fold_root(true) FROM public.folds f WHERE f.name = same.name$$;

-- since(counter): pg_stat_prolly's counter's growth since it was last
-- asked, the session's counts flushed first (:flush, a statement of its
-- own).
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION pg_temp.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;

-- A status after a row written takes both tables' descriptions as it reads
-- the definitions, and each table's again as it reads that table's
-- changes, and the check's fold takes both again; a commit's fold takes
-- both.
UPDATE dc.u SET w = 'c' WHERE id = 1;
SELECT count(*) AS changes FROM dolt_status;
:flush
SELECT pg_temp.since('describe_cache_hits') >= 0 AS counting;
UPDATE dc.u SET w = 'd' WHERE id = 1;
SELECT count(*) AS changes FROM dolt_status;
:flush
SELECT pg_temp.since('describe_cache_hits') AS status_taken;
SELECT length(dolt_commit('-am', 'd')::text) > 0 AS committed;
:flush
SELECT pg_temp.since('describe_cache_hits') AS taken;

-- The same with the check off: the incremental fold's root is the full
-- one's, with the caches on and with them checked.
CREATE FUNCTION public.same_as_full() RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	inc text;
	full_root text;
BEGIN
	inc := test_fold_root();
	SET LOCAL debug_prolly_force_full_fold = on;
	full_root := test_fold_root();
	SET LOCAL debug_prolly_force_full_fold = off;
	RETURN inc = full_root;
END $$;
UPDATE dc.t SET v = 'e' WHERE id = 1;
SET debug_prolly_verify_fold_caches = off;
SELECT public.same_as_full() AS same_cached;
RESET debug_prolly_verify_fold_caches;
SELECT public.same_as_full() AS same_checked;
SELECT length(dolt_commit('-am', 'e')::text) > 0 AS committed;

-- Each change, with the check off, then a fresh session's fold.
SET debug_prolly_verify_fold_caches = off;
SELECT count(*) AS changes FROM dolt_status;
ALTER TABLE dc.t ADD COLUMN x int DEFAULT 7;
CALL public.keep('alter table');
\c
SELECT public.same('alter table');

SET debug_prolly_verify_fold_caches = off;
SELECT count(*) AS changes FROM dolt_status;
CREATE INDEX t_v ON dc.t (v);
CALL public.keep('create index');
\c
SELECT public.same('create index');

-- (the trigger's function renamed sends its table nothing)
SET debug_prolly_verify_fold_caches = off;
SELECT count(*) AS changes FROM dolt_status;
ALTER FUNCTION dc.touch() RENAME TO touched;
CALL public.keep('trigger function renamed');
\c
SELECT public.same('trigger function renamed');

-- (an enum's label added, or renamed, changes how its table's rows are
-- read: the baseline's tree for them isn't theirs)
SET debug_prolly_verify_fold_caches = off;
SELECT count(*) AS changes FROM dolt_status;
ALTER TYPE dc.mood ADD VALUE 'glad';
CALL public.keep('enum value added');
\c
SELECT public.same('enum value added');
SET debug_prolly_verify_fold_caches = off;
SET dolt.author = 'Ada Lovelace <ada@example.com>';
SELECT length(dolt_commit('-Am', 'glad')::text) > 0 AS committed;
ALTER TYPE dc.mood RENAME VALUE 'sad' TO 'blue';
CALL public.keep('enum value renamed');
\c
SELECT public.same('enum value renamed');

SET debug_prolly_verify_fold_caches = off;
SELECT count(*) AS changes FROM dolt_status;
ALTER DOMAIN dc.small RENAME TO tiny;
ALTER DOMAIN dc.tiny ADD CONSTRAINT tiny_pos CHECK (VALUE > 0);
CALL public.keep('domain altered');
\c
SELECT public.same('domain altered');

-- The transaction's own DDL: its fold sees it, and a fold after it is
-- rolled back (or its savepoint is) doesn't.
SET debug_prolly_verify_fold_caches = off;
SELECT count(*) AS changes FROM dolt_status;
BEGIN;
ALTER TABLE dc.u ADD COLUMN y text;
SELECT table_name, status FROM dolt_status ORDER BY table_name;
SET LOCAL debug_prolly_verify_fold_caches = on;
SELECT count(*) AS changes FROM dolt_status;
ROLLBACK;
SELECT count(*) AS changes FROM dolt_status;
CALL public.keep('rolled back');
\c
SELECT public.same('rolled back');
SET debug_prolly_verify_fold_caches = off;
SELECT count(*) AS changes FROM dolt_status;
BEGIN;
SAVEPOINT s;
ALTER TABLE dc.u ADD COLUMN z text;
SELECT count(*) AS changes FROM dolt_status;
ROLLBACK TO SAVEPOINT s;
SELECT count(*) AS changes FROM dolt_status;
COMMIT;
CALL public.keep('savepoint rolled back');
\c
SELECT public.same('savepoint rolled back');

-- A branch switch: the other branch's table described there, then this
-- one's again.
SET debug_prolly_verify_fold_caches = off;
SET dolt.author = 'Ada Lovelace <ada@example.com>';
SELECT length(dolt_commit('-Am', 'before other')::text) > 0 AS committed;
SELECT dolt_branch('other');
SELECT dolt_checkout('other');
ALTER TABLE dc.u ADD COLUMN on_other int;
SELECT length(dolt_commit('-Am', 'on other')::text) > 0 AS committed;
SELECT dolt_checkout('main');
SELECT count(*) AS changes FROM dolt_status;
UPDATE dc.u SET w = 'f' WHERE id = 1;
CALL public.keep('branch switch');
\c
SELECT public.same('branch switch');

-- What the versioned objects depend on: an ordinary schema's type, then
-- an extension's member (ALTER EXTENSION ADD changes only pg_depend);
-- a function named DEPENDS ON EXTENSION.
SET debug_prolly_verify_fold_caches = off;
SET dolt.author = 'Ada Lovelace <ada@example.com>';
SET client_min_messages = warning;
CREATE EXTENSION test_prolly_b;
RESET client_min_messages;
CREATE TYPE public.hue AS ENUM ('red', 'blue');
CREATE FUNCTION dc.one() RETURNS int LANGUAGE sql AS 'SELECT 1';
CREATE TABLE dc.h (id int PRIMARY KEY, c public.hue);
SELECT length(dolt_commit('-Am', 'hue')::text) > 0 AS committed;
SELECT unnest(test_commit_dependencies(dolt_hashof('HEAD'))) AS dependency;
ALTER EXTENSION test_prolly_b ADD TYPE public.hue;
SELECT length(dolt_commit('--allow-empty', '-Am', 'member')::text) > 0 AS committed;
SELECT unnest(test_commit_dependencies(dolt_hashof('HEAD'))) AS dependency;
ALTER EXTENSION test_prolly_b DROP TYPE public.hue;
ALTER FUNCTION dc.one() DEPENDS ON EXTENSION test_prolly_b;
SELECT length(dolt_commit('--allow-empty', '-Am', 'depends')::text) > 0 AS committed;
SELECT unnest(test_commit_dependencies(dolt_hashof('HEAD'))) AS dependency;

-- An object whose rename tells no table and no pg_depend row of it (a
-- text search configuration's): a description printing its name (a
-- generated column's), and the dependency on it, are worked out again.
CREATE TEXT SEARCH CONFIGURATION public.cfg (COPY = pg_catalog.simple);
CREATE TABLE dc.d (id int PRIMARY KEY, body text,
  tsv tsvector GENERATED ALWAYS AS (to_tsvector('public.cfg'::regconfig, body)) STORED);
INSERT INTO dc.d VALUES (1, 'hello');
SELECT length(dolt_commit('-Am', 'cfg')::text) > 0 AS committed;
UPDATE dc.d SET body = 'x';
SELECT length(dolt_commit('-am', 'warm')::text) > 0 AS committed;
UPDATE dc.d SET body = 'y';
ALTER TEXT SEARCH CONFIGURATION public.cfg RENAME TO cfg2;
CALL public.keep('configuration renamed');
SELECT length(dolt_commit('-am', 'renamed')::text) > 0 AS committed;
SELECT dependency FROM unnest(test_commit_dependencies(dolt_hashof('HEAD'))) dependency
WHERE dependency LIKE 'text search%';
\c
SELECT public.same('configuration renamed');

\c contrib_regression
DROP DATABASE regress_describe_caches;
