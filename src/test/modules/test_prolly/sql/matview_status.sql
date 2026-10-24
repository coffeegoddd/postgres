-- A status reads definitions without rows, so a materialized view's rows
-- are its baseline's.  A plain REFRESH rewrites them and folds the branch,
-- so the baseline holds them; a concurrent one changes them in place, as
-- writes to a table do, and the status must still see the change.  A
-- materialized view with no change since its baseline keeps the status
-- from folding (fold_tables).  Run with debug_prolly_verify_fold_caches
-- on (the suite's) and off.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
\set reset 'SELECT pg_stat_force_next_flush(); SELECT pg_stat_reset_shared(''prolly'');'
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE FUNCTION ms_folded() RETURNS bigint LANGUAGE sql AS $$
	SELECT value FROM pg_stat_prolly WHERE name = 'fold_tables'
$$;

CREATE SCHEMA ms VERSIONED;
CREATE TABLE ms.t (pk int PRIMARY KEY, v int);
INSERT INTO ms.t SELECT g, g FROM generate_series(1, 5) g;
CREATE MATERIALIZED VIEW ms.mv AS SELECT pk, v FROM ms.t;
CREATE UNIQUE INDEX ON ms.mv (pk);
SELECT length(dolt_commit('-Am', 'base')::text) > 0 AS committed;

-- verify on: the change finder's answer is checked against a fold
UPDATE ms.t SET v = v + 1 WHERE pk = 1;
SELECT * FROM dolt_status ORDER BY 1;
REFRESH MATERIALIZED VIEW CONCURRENTLY ms.mv;
SELECT * FROM dolt_status ORDER BY 1;
SELECT dolt_reset('--hard') IS NOT NULL AS reset;

-- verify off: the change finder's answer alone
SET debug_prolly_verify_fold_caches = off;
UPDATE ms.t SET v = v + 1 WHERE pk = 2;
:reset
SELECT * FROM dolt_status ORDER BY 1;
:flush
SELECT ms_folded() AS view_unchanged_folded;
REFRESH MATERIALIZED VIEW CONCURRENTLY ms.mv;
SELECT * FROM dolt_status ORDER BY 1;
SELECT dolt_reset('--hard') IS NOT NULL AS reset;
RESET debug_prolly_verify_fold_caches;

-- A status after a status takes the last one's answer, asking again only
-- what was written since: a materialized view refreshed concurrently in
-- between is one of those.
SET debug_prolly_verify_fold_caches = off;
UPDATE ms.t SET v = v + 1 WHERE pk = 3;
SELECT * FROM dolt_status ORDER BY 1;
SELECT * FROM dolt_status ORDER BY 1;
REFRESH MATERIALIZED VIEW CONCURRENTLY ms.mv;
SELECT * FROM dolt_status ORDER BY 1;
SELECT dolt_reset('--hard') IS NOT NULL AS reset;
RESET debug_prolly_verify_fold_caches;
UPDATE ms.t SET v = v + 1 WHERE pk = 3;
SELECT * FROM dolt_status ORDER BY 1;
SELECT * FROM dolt_status ORDER BY 1;
REFRESH MATERIALIZED VIEW CONCURRENTLY ms.mv;
SELECT * FROM dolt_status ORDER BY 1;
SELECT dolt_reset('--hard') IS NOT NULL AS reset;

-- A keyless row too long for its log entry is an entry to read the table
-- in full: not a change the log can rule out.  A keyless table, and a
-- materialized view refreshed concurrently with such a row; a hard reset
-- then undoes the rows.
CREATE TABLE ms.k (v text STORAGE PLAIN);
CREATE TABLE ms.wide (pk int PRIMARY KEY, w text);
CREATE MATERIALIZED VIEW ms.kv AS SELECT pk, w FROM ms.wide;
ALTER MATERIALIZED VIEW ms.kv ALTER COLUMN w SET STORAGE PLAIN;
CREATE UNIQUE INDEX ON ms.kv (pk);
SELECT length(dolt_commit('-Am', 'wide rows')::text) > 0 AS committed;
INSERT INTO ms.k VALUES (repeat('x', 8100));
INSERT INTO ms.wide VALUES (1, repeat('y', 8100));
SELECT * FROM dolt_status ORDER BY 1;
REFRESH MATERIALIZED VIEW CONCURRENTLY ms.kv;
SELECT * FROM dolt_status ORDER BY 1;
SET debug_prolly_verify_fold_caches = off;
SELECT * FROM dolt_status ORDER BY 1;
RESET debug_prolly_verify_fold_caches;
SELECT dolt_reset('--hard') IS NOT NULL AS reset;
SELECT (SELECT count(*) FROM ms.k) AS k_rows,
	(SELECT count(*) FROM ms.kv) AS kv_rows;
SELECT * FROM dolt_status ORDER BY 1;

DROP SCHEMA ms CASCADE;
DROP FUNCTION ms_folded();
SELECT length(dolt_commit('-Am', 'ms gone')::text) > 0 AS committed;
