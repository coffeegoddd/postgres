-- pg_stat_prolly: the engine's work, counted per database.  The same work
-- counts the same, exactly: a commit of rows writes so many nodes, and
-- every node it writes is a chunk appended.

-- A database of its own, so nothing else counts in it.
SELECT current_database() AS home \gset
CREATE DATABASE stat_prolly;
\c stat_prolly
SET dolt.author = 'Ada Lovelace <ada@example.com>';
CREATE SCHEMA sp VERSIONED;
CREATE TABLE sp.t (id int PRIMARY KEY, v text);
SELECT length(dolt_commit('-Am', 'tables')::text) > 0 AS committed;
-- (what this session counted so far is flushed before the reset)
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT name, value FROM pg_stat_prolly WHERE value <> 0;

-- A commit of 1000 new rows: the fold reads and writes each.
INSERT INTO sp.t SELECT g, 'v' || g FROM generate_series(1, 1000) g;
SELECT length(dolt_commit('-m', '1000 rows')::text) > 0 AS committed;
SELECT pg_stat_force_next_flush();
SELECT name, value FROM pg_stat_prolly;
SELECT (SELECT value FROM pg_stat_prolly WHERE name = 'chunks_appended') =
	(SELECT value FROM pg_stat_prolly WHERE name = 'nodes_written') AS each_node_a_chunk,
	(SELECT value FROM pg_stat_prolly WHERE name = 'bytes_hashed') >=
	(SELECT value FROM pg_stat_prolly WHERE name = 'chunk_bytes_appended') AS each_chunk_hashed;

-- One row changed: the fold reads its page's rows and writes the one.
SELECT pg_stat_reset_shared('prolly');
UPDATE sp.t SET v = 'x' WHERE id = 500;
SELECT length(dolt_commit('-m', 'one row')::text) > 0 AS committed;
SELECT pg_stat_force_next_flush();
SELECT name, value FROM pg_stat_prolly
WHERE name IN ('fold_rows_written', 'fold_full_tables', 'commits', 'head_moves');

-- The same commit again, in a database just like it: the same counts.
SELECT array_agg(value ORDER BY name) AS first FROM pg_stat_prolly \gset
\c :home
CREATE DATABASE stat_prolly_again;
\c stat_prolly_again
SET dolt.author = 'Ada Lovelace <ada@example.com>';
CREATE SCHEMA sp VERSIONED;
CREATE TABLE sp.t (id int PRIMARY KEY, v text);
SELECT length(dolt_commit('-Am', 'tables')::text) > 0 AS committed;
INSERT INTO sp.t SELECT g, 'v' || g FROM generate_series(1, 1000) g;
SELECT length(dolt_commit('-m', '1000 rows')::text) > 0 AS committed;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
UPDATE sp.t SET v = 'x' WHERE id = 500;
SELECT length(dolt_commit('-m', 'one row')::text) > 0 AS committed;
SELECT pg_stat_force_next_flush();
SELECT array_agg(value ORDER BY name) = :'first' AS same_counts FROM pg_stat_prolly;

-- Per database: the other's work isn't counted here.
\c stat_prolly
SELECT sum(value) = 0 AS nothing_since_reset FROM pg_stat_prolly;
-- An unknown target is still refused, and names this one.
SELECT pg_stat_reset_shared('prollyx');

\c :home
DROP DATABASE stat_prolly;
DROP DATABASE stat_prolly_again;
