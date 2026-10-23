-- A fold from the baseline whose changes pass maintenance_work_mem sorts
-- them, spilling to disk, and applies them a part at a time
-- (fold_changes_spilled), rather than reading the table in full: its work
-- stays that of the rows changed, whatever the table's size.  Each fold
-- here is checked against a fold of every row.

CREATE DATABASE spilled_folds;
\c spilled_folds
CREATE EXTENSION test_prolly;
\set reset 'SELECT pg_stat_force_next_flush(); SELECT pg_stat_reset_shared(''prolly'');'
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE FUNCTION work() RETURNS TABLE (rows_read bigint, full_reads bigint,
	spilled bigint, merges bigint) LANGUAGE sql AS $$
	SELECT max(value) FILTER (WHERE name = 'fold_rows_read'),
		max(value) FILTER (WHERE name = 'fold_full_tables'),
		max(value) FILTER (WHERE name = 'fold_changes_spilled'),
		max(value) FILTER (WHERE name = 'fold_merge_builds')
	FROM pg_stat_prolly
$$;
CREATE FUNCTION full_folds() RETURNS TABLE (reason text, n bigint) LANGUAGE sql AS $$
	SELECT substr(name, 11), value FROM pg_stat_prolly
	WHERE name LIKE 'fold\_full\_%' AND name NOT IN ('fold_full_tables', 'fold_full_spilled')
		AND value > 0
	ORDER BY name
$$;
-- (the fold of every row run after the counted one)
CREATE FUNCTION same_as_full() RETURNS bool LANGUAGE sql AS $$
	SELECT test_fold_root() = test_fold_root(true)
$$;
CREATE SCHEMA sp VERSIONED;

-- The same 2,000 keys changed (their entries past 64kB together), in
-- tables of 10,000 and 100,000 rows: 2,000 rows read either way, none in
-- full (the smaller's, past a tenth of its rows, merged with the baseline
-- in one pass).  Beneath the budget (200 keys), nothing is sorted.
CREATE TABLE sp.small (id int PRIMARY KEY, v text);
INSERT INTO sp.small SELECT g, md5(g::text) FROM generate_series(1, 10000) g;
CREATE TABLE sp.large (id int PRIMARY KEY, v text);
INSERT INTO sp.large SELECT g, md5(g::text) FROM generate_series(1, 100000) g;
SELECT length(dolt_commit('-Am', 'base')::text) > 0 AS committed;
SET maintenance_work_mem = '64kB';
UPDATE sp.small SET v = 'past' WHERE id % 5 = 0;
:reset
SELECT test_fold_root() IS NOT NULL AS folded;
:flush
SELECT 'small, past' AS fold, * FROM work();
SELECT same_as_full() AS same;
SELECT length(dolt_commit('-am', 'small')::text) > 0 AS committed;
UPDATE sp.large SET v = 'past' WHERE id % 50 = 0;
:reset
SELECT test_fold_root() IS NOT NULL AS folded;
:flush
SELECT 'large, past' AS fold, * FROM work();
SELECT same_as_full() AS same;
SELECT length(dolt_commit('-am', 'large')::text) > 0 AS committed;
UPDATE sp.large SET v = 'beneath' WHERE id % 500 = 0;
:reset
SELECT test_fold_root() IS NOT NULL AS folded;
:flush
SELECT 'large, beneath' AS fold, * FROM work();
SELECT same_as_full() AS same;
SELECT length(dolt_commit('-am', 'beneath')::text) > 0 AS committed;

-- A key written many times, keys deleted, keys changed, rows inserted,
-- and values stored out of line: each row read once, as the snapshot sees
-- it.
UPDATE sp.large SET v = repeat('a', 300) || id WHERE id <= 300;
UPDATE sp.large SET v = repeat('b', 300) || id WHERE id <= 300;
UPDATE sp.large SET v = 'last' WHERE id = 7;
UPDATE sp.large SET v = 'again' WHERE id = 7;
DELETE FROM sp.large WHERE id BETWEEN 301 AND 500;
UPDATE sp.large SET id = id + 1000000 WHERE id BETWEEN 501 AND 600;
INSERT INTO sp.large SELECT g, repeat('n', 250) FROM generate_series(200001, 200300) g;
UPDATE sp.large SET v = repeat(md5(id::text), 400) WHERE id BETWEEN 601 AND 620;
:reset
SELECT test_fold_root() IS NOT NULL AS folded;
:flush
SELECT rows_read < 4000 AS proportional, full_reads, spilled FROM work();
SELECT same_as_full() AS same;
SELECT length(dolt_commit('-am', 'mixed')::text) > 0 AS committed;
SELECT diff_type, count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::sp.large)
GROUP BY 1 ORDER BY 1;

-- Keyless rows: copies made and taken, counted once per row, a row's
-- copies never split between the parts applied.
CREATE TABLE sp.bag (a int, v text);
INSERT INTO sp.bag SELECT g % 2000, md5((g % 2000)::text) FROM generate_series(1, 20000) g;
SELECT length(dolt_commit('-Am', 'bag')::text) > 0 AS committed;
INSERT INTO sp.bag SELECT g % 300, md5((g % 300)::text) FROM generate_series(1, 3000) g;
DELETE FROM sp.bag WHERE ctid IN (SELECT ctid FROM sp.bag WHERE a BETWEEN 300 AND 700 AND a % 2 = 0 LIMIT 1500);
INSERT INTO sp.bag SELECT g, repeat('w', 250) FROM generate_series(5000, 5400) g;
INSERT INTO sp.bag SELECT 9999, 'once' FROM generate_series(1, 5);
DELETE FROM sp.bag WHERE a = 9999;
:reset
SELECT test_fold_root() IS NOT NULL AS folded;
:flush
SELECT rows_read < 20000 AS proportional, full_reads, spilled FROM work();
SELECT same_as_full() AS same;
SELECT length(dolt_commit('-am', 'bag changed')::text) > 0 AS committed;
SELECT count(*) AS rows_at_head FROM sp.bag;

-- Many rows changed: past a tenth of the table's, merged with the baseline
-- in one pass, from the part where they pass it.
UPDATE sp.small SET v = 'dense' WHERE id % 10 < 3;
:reset
SELECT test_fold_root() IS NOT NULL AS folded;
:flush
SELECT full_reads, spilled, merges FROM work();
SELECT same_as_full() AS same;
SELECT length(dolt_commit('-am', 'dense')::text) > 0 AS committed;

-- The table emptied, and filled again: from no rows, every key a
-- candidate.
TRUNCATE sp.small;
INSERT INTO sp.small SELECT g, 'again' FROM generate_series(1, 3000) g;
:reset
SELECT test_fold_root() IS NOT NULL AS folded;
:flush
SELECT full_reads, spilled FROM work();
SELECT same_as_full() AS same;
SELECT length(dolt_commit('-am', 'reloaded')::text) > 0 AS committed;
RESET maintenance_work_mem;

-- More keyless rows read back from the sort than the table held (and
-- past 65,536): read in full, as that costs less.
CREATE TABLE sp.few (a int, v text);
INSERT INTO sp.few VALUES (1, 'one');
ANALYZE sp.few;
SELECT length(dolt_commit('-Am', 'few')::text) > 0 AS committed;
-- (two transactions: one past 65,536 rows is noted as a bulk write)
INSERT INTO sp.few SELECT g, 'many' FROM generate_series(1, 35000) g;
INSERT INTO sp.few SELECT g, 'more' FROM generate_series(1, 35000) g;
SET maintenance_work_mem = '64kB';
:reset
SELECT test_fold_root() IS NOT NULL AS folded;
:flush
SELECT * FROM full_folds();
SELECT same_as_full() AS same;
RESET maintenance_work_mem;
DROP TABLE sp.few;
SELECT length(dolt_commit('-Am', 'few gone')::text) > 0 AS committed;

-- A transaction's own rows committed by dolt.transaction_commit, past
-- maintenance_work_mem: its working set committed instead, folded from the
-- baseline, every row of it.
SET dolt.transaction_commit = on;
SET maintenance_work_mem = '64kB';
BEGIN;
INSERT INTO sp.small SELECT g, repeat('o', 300) || g FROM generate_series(5001, 6000) g;
COMMIT;
RESET maintenance_work_mem;
RESET dolt.transaction_commit;
SELECT rows_added FROM dolt_diff_stat('HEAD~1', 'HEAD') WHERE table_name = 'sp.small';
SELECT count(*) AS changes FROM dolt_status;

\c contrib_regression
DROP DATABASE spilled_folds;
