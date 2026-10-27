-- A VACUUM of a table whose pages a branch may share freezes its tuples as
-- soon as every snapshot sees them (freeze age 0), while its pages are
-- still its own: frozen in place, then shared all-frozen, so no VACUUM
-- after the share copies a page to freeze it.  A setting given explicitly
-- (vacuum_freeze_min_age, or the autovacuum_freeze_min_age reloption)
-- still has its say, and a heap table is vacuumed as ever.  Pages already
-- all-visible (as reads set them, on-access pruning: here, all of t's
-- but the last, by the INSERT ... SELECTs) are scanned to freeze them
-- too, unless the eager scan's failure rate was set.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT current_database() AS home \gset
CREATE DATABASE eager_freeze;
-- (pages shared with a branch alone: pages kept as of each commit are shared too, which VACUUM leaves as they are)
ALTER DATABASE eager_freeze SET debug_prolly_page_roots = off;
\c eager_freeze
CREATE EXTENSION pageinspect;
CREATE SCHEMA ef VERSIONED;
CREATE TABLE ef.t (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
CREATE TABLE ef.s (LIKE ef.t) WITH (autovacuum_enabled = off);
CREATE TABLE ef.e (LIKE ef.t) WITH (autovacuum_enabled = off);
CREATE TABLE ef.r (LIKE ef.t)
	WITH (autovacuum_enabled = off, autovacuum_freeze_min_age = 1000000);
CREATE TABLE public.h (LIKE ef.t) USING heap WITH (autovacuum_enabled = off);
INSERT INTO ef.t SELECT g, repeat('v', 100) FROM generate_series(1, 2000) g;
INSERT INTO ef.s SELECT * FROM ef.t;
INSERT INTO ef.r SELECT * FROM ef.t;
INSERT INTO ef.e SELECT * FROM ef.t;
INSERT INTO public.h SELECT * FROM ef.t;
SELECT length(dolt_commit('-Am', 'rows')::text) > 0 AS committed;

-- How many of a table's tuples, on its first page, are frozen.
CREATE FUNCTION frozen(rel regclass) RETURNS text LANGUAGE sql AS $$
	SELECT count(*) FILTER (WHERE t_infomask & 768 = 768) || ' of ' || count(*)
	FROM heap_page_items(get_raw_page(rel::text, 0))
$$;

VACUUM (PROCESS_TOAST false) ef.t;
SELECT frozen('ef.t') AS prolly_frozen;
VACUUM public.h;
SELECT frozen('public.h') AS heap_not_frozen;
VACUUM (PROCESS_TOAST false) ef.r;
SELECT frozen('ef.r') AS reloption_not_frozen;
SET vacuum_freeze_min_age = 1000000;
VACUUM (PROCESS_TOAST false) ef.s;
SELECT frozen('ef.s') AS setting_not_frozen;
RESET vacuum_freeze_min_age;
-- Pages already all-visible, not frozen (by a VACUUM that the setting
-- kept from freezing them), are scanned to freeze them; with the eager
-- scan's failure rate set, they are left to the eager scan, which a table
-- this small doesn't do.
SET vacuum_freeze_min_age = 1000000;
VACUUM (PROCESS_TOAST false) ef.e;
RESET vacuum_freeze_min_age;
SET vacuum_max_eager_freeze_failure_rate = 0.03;
VACUUM (PROCESS_TOAST false) ef.e;
SELECT frozen('ef.e') AS failure_rate_not_frozen;
RESET vacuum_max_eager_freeze_failure_rate;
VACUUM (PROCESS_TOAST false) ef.e;
SELECT frozen('ef.e') AS all_visible_frozen;

-- (the branch's first use: its copies share the tables' pages)
SELECT dolt_branch('other');
SELECT dolt_checkout('other');
SELECT count(*) FROM ef.t;
SELECT dolt_checkout('main');

-- An aggressive VACUUM of either branch copies none of t's pages to
-- freeze them: they were shared all-frozen.  s's, shared unfrozen, are
-- copied.
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
VACUUM (FREEZE, PROCESS_TOAST false) ef.t;
SELECT pg_stat_force_next_flush();
SELECT value AS copied_on_main FROM pg_stat_prolly WHERE name = 'page_copies_shared';
SELECT dolt_checkout('other');
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
VACUUM (FREEZE, PROCESS_TOAST false) ef.t;
SELECT pg_stat_force_next_flush();
SELECT value AS copied_on_branch FROM pg_stat_prolly WHERE name = 'page_copies_shared';
SELECT frozen('ef.t') AS branch_frozen;
SELECT dolt_checkout('main');
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
VACUUM (FREEZE, PROCESS_TOAST false) ef.s;
SELECT pg_stat_force_next_flush();
SELECT value > 0 AS copied_unfrozen FROM pg_stat_prolly WHERE name = 'page_copies_shared';

-- Both branches read as they were.
SELECT count(*), sum(length(v)) FROM ef.t;
SELECT dolt_checkout('other');
SELECT count(*), sum(length(v)) FROM ef.t;
SELECT dolt_checkout('main');

\c :home
DROP DATABASE eager_freeze;
