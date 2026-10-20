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

\c :home
DROP DATABASE vacuum_shared;
