-- A VACUUM leaves a page another branch shares as it is (copy-on-write,
-- pruning, freezing or setting it all-visible would copy it), unless it
-- must freeze it: a plain VACUUM after a branch's first use copies none,
-- where it copied every page it set all-visible; VACUUM (FREEZE), which
-- must, still copies them.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT current_database() AS home \gset
CREATE DATABASE vacuum_shared;
-- (pages shared with a branch alone: pages kept as of each commit are shared too, which VACUUM leaves as they are)
ALTER DATABASE vacuum_shared SET debug_prolly_page_roots = off;
\c vacuum_shared
CREATE SCHEMA vs VERSIONED;
CREATE TABLE vs.t (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
INSERT INTO vs.t SELECT g, repeat('v', 100) FROM generate_series(1, 2000) g;
SELECT length(dolt_commit('-Am', 'rows')::text) > 0 AS committed;
-- (the branch's first use: its copy shares the table's pages)
SELECT dolt_branch('other');
SELECT dolt_checkout('other');
SELECT count(*) FROM vs.t;
SELECT dolt_checkout('main');
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');

VACUUM vs.t;
SELECT pg_stat_force_next_flush();
SELECT value AS copied_by_vacuum FROM pg_stat_prolly WHERE name = 'page_copies_shared';

VACUUM (FREEZE) vs.t;
SELECT pg_stat_force_next_flush();
SELECT value > 0 AS copied_to_freeze FROM pg_stat_prolly WHERE name = 'page_copies_shared';

-- Both branches read as they were.
SELECT count(*), sum(length(v)) FROM vs.t;
SELECT dolt_checkout('other');
SELECT count(*), sum(length(v)) FROM vs.t;
SELECT dolt_checkout('main');

-- On the branch, a plain VACUUM leaves the pages it shares too.
SELECT dolt_checkout('other');
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
VACUUM vs.t;
SELECT pg_stat_force_next_flush();
SELECT value AS copied_on_branch FROM pg_stat_prolly WHERE name = 'page_copies_shared';
SELECT dolt_checkout('main');

-- A shared page holding dead tuples is pruned, copied: the space is
-- freed, and the dead aren't counted again at the next VACUUM (left, they
-- would be at every one, and autovacuum would come back for them).  The
-- pages holding none are left.
CREATE TABLE vs.d (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
INSERT INTO vs.d SELECT g, repeat('d', 100) FROM generate_series(1, 2000) g;
SELECT length(dolt_commit('-Am', 'rows')::text) > 0 AS committed;
-- (rows a rolled-back insert left: dead, and never pruned on access, which
-- only prunes pages a delete or update marked prunable)
BEGIN;
INSERT INTO vs.d SELECT g, repeat('d', 100) FROM generate_series(2001, 2300) g;
ROLLBACK;
SELECT dolt_branch('dead');
SELECT dolt_checkout('dead');
SELECT count(*) FROM vs.d;
SELECT dolt_checkout('main');
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
-- (the table alone: its change log, vacuumed with it as its TOAST table
-- is, has dead entries of its own to free)
VACUUM (PROCESS_TOAST false) vs.d;
SELECT pg_stat_force_next_flush();
SELECT value > 0 AND value < pg_relation_size('vs.d') / current_setting('block_size')::int / 2
	AS copied_only_dead_pages
FROM pg_stat_prolly WHERE name = 'page_copies_shared';
SELECT n_dead_tup FROM pg_stat_user_tables WHERE relid = 'vs.d'::regclass;
VACUUM vs.d;
SELECT pg_stat_force_next_flush();
SELECT n_dead_tup FROM pg_stat_user_tables WHERE relid = 'vs.d'::regclass;
SELECT count(*), max(id) FROM vs.d;
SELECT dolt_checkout('dead');
SELECT count(*), max(id) FROM vs.d;
SELECT dolt_checkout('main');

\c :home
DROP DATABASE vacuum_shared;
