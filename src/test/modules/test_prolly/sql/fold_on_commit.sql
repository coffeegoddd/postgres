-- prolly.fold_on_commit: each transaction's own row changes folded into its
-- branch's baseline as it commits, so a status, a diff into WORKING and a
-- commit fold nothing of them after.  (This suite runs with
-- debug_prolly_verify_fold_caches on: each status and diff read from
-- changes is read from a fold too, and compared.)

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT current_database() AS home \gset
CREATE DATABASE fold_on_commit;
\c fold_on_commit
SET dolt.author = 'Ada Lovelace <ada@example.com>';
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION pg_temp.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;

CREATE SCHEMA fc VERSIONED;
CREATE TABLE fc.t (id int PRIMARY KEY, v text);
CREATE TABLE fc.k (v text);					-- keyless
INSERT INTO fc.t SELECT g, 'v' || g FROM generate_series(1, 2000) g;
INSERT INTO fc.k VALUES ('a'), ('a'), ('b');
SELECT length(dolt_commit('-Am', 'base')::text) > 0 AS committed;
SET prolly.fold_on_commit = on;

-- Rows written: each transaction's folded as it commits.
UPDATE fc.t SET v = 'one' WHERE id = 1;
INSERT INTO fc.t VALUES (5000, 'new');
DELETE FROM fc.t WHERE id = 2;
INSERT INTO fc.k VALUES ('a');
DELETE FROM fc.k WHERE v = 'b';
BEGIN;
UPDATE fc.t SET v = 'kept' WHERE id = 3;
SAVEPOINT s;
UPDATE fc.t SET v = 'rolled back' WHERE id = 4;
ROLLBACK TO s;
COMMIT;
BEGIN;
UPDATE fc.t SET v = 'never' WHERE id = 5;
ROLLBACK;

-- Nothing left to fold: a status, a diff and a commit read no row of them
-- (the check off, to show it).
SET debug_prolly_verify_fold_caches = off;
:flush
SELECT pg_temp.since('fold_rows_read') IS NOT NULL AS counting;
SELECT * FROM dolt_status ORDER BY 1;
SELECT diff_type, (from_row).id, (to_row).id, (to_row).v
FROM dolt_diff('HEAD', 'WORKING', NULL::fc.t) ORDER BY coalesce((from_row).id, (to_row).id);
SELECT diff_type, (from_row).v, (to_row).v
FROM dolt_diff('HEAD', 'WORKING', NULL::fc.k) ORDER BY 2, 3;
:flush
SELECT pg_temp.since('fold_rows_read') AS rows_read_after;
RESET debug_prolly_verify_fold_caches;

-- The same, checked against folds (the check on).
SELECT * FROM dolt_status ORDER BY 1;
SELECT count(*) AS changed FROM dolt_diff('HEAD', 'WORKING', NULL::fc.t);

SET debug_prolly_verify_fold_caches = off;
:flush
SELECT pg_temp.since('fold_rows_read') IS NOT NULL AS counting;
SELECT length(dolt_commit('-am', 'folded')::text) > 0 AS committed;
:flush
SELECT pg_temp.since('fold_rows_read') AS commit_rows_read;
RESET debug_prolly_verify_fold_caches;
SELECT diff_type, (from_row).id, (to_row).id
FROM dolt_diff('HEAD~1', 'HEAD', NULL::fc.t) ORDER BY coalesce((from_row).id, (to_row).id);
SELECT * FROM dolt_status;

-- A transaction that changed definitions isn't folded: its changes are
-- the next fold's, as without the mode.
BEGIN;
ALTER TABLE fc.t ADD COLUMN w int;
UPDATE fc.t SET v = 'with ddl' WHERE id = 6;
COMMIT;
SELECT * FROM dolt_status ORDER BY 1;
SELECT count(*) AS changed FROM dolt_diff('HEAD', 'WORKING', NULL::fc.t);
SELECT length(dolt_commit('-am', 'ddl')::text) > 0 AS committed;

-- Off again: the next fold folds them.
RESET prolly.fold_on_commit;
UPDATE fc.t SET v = 'lazy' WHERE id = 7;
SELECT * FROM dolt_status;
SELECT length(dolt_commit('-am', 'lazy')::text) > 0 AS committed;
SELECT * FROM dolt_status;

\c :home
DROP DATABASE fold_on_commit;
