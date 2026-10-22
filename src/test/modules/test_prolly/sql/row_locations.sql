-- A change log's key entry holds where the key's version was written, so a
-- fold reads the row there (fold_rows_located) rather than look the key up;
-- where the version there isn't the snapshot's, or has another key (updated
-- since, deleted, its place reused after VACUUM), the key is looked up, as
-- before.  (The fold verifier checks each fold against a full one.)

SELECT current_database() AS home \gset
CREATE DATABASE row_locations;
\c row_locations
SET dolt.author = 'Ada Lovelace <ada@example.com>';
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION pg_temp.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE FUNCTION pg_temp.check() RETURNS boolean LANGUAGE sql AS $$
	SELECT test_fold_root() = test_fold_root(true)
$$;

CREATE SCHEMA rl VERSIONED;
CREATE TABLE rl.t (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
INSERT INTO rl.t SELECT g, 'v' || g FROM generate_series(1, 1000) g;
SELECT length(dolt_commit('-Am', 'base')::text) > 0 AS committed;

-- Rows updated once: each read where its entry says.
UPDATE rl.t SET v = 'one' WHERE id <= 100;
:flush
SELECT pg_temp.since('fold_rows_located') IS NOT NULL AS counting;
SELECT length(dolt_commit('-am', 'one')::text) > 0 AS committed;
:flush
SELECT pg_temp.since('fold_rows_located') >= 100 AS located;
SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::rl.t);

-- A key updated again in another transaction, its first entry's version
-- gone: looked up, or read where the later entry says; the same rows.
UPDATE rl.t SET v = 'two' WHERE id <= 10;
UPDATE rl.t SET v = 'three' WHERE id <= 5;
DELETE FROM rl.t WHERE id BETWEEN 6 AND 8;
UPDATE rl.t SET id = id + 5000 WHERE id = 9;	-- the key changed
SELECT pg_temp.check() AS same_as_full;
SELECT diff_type, (from_row).id, (to_row).id, (to_row).v
FROM dolt_diff('HEAD', 'WORKING', NULL::rl.t) ORDER BY coalesce((from_row).id, (to_row).id);
SELECT length(dolt_commit('-am', 'mixed')::text) > 0 AS committed;
SELECT diff_type, (from_row).id, (to_row).id, (to_row).v
FROM dolt_diff('HEAD~1', 'HEAD', NULL::rl.t) ORDER BY coalesce((from_row).id, (to_row).id);

-- Places reused: rows updated, vacuumed, others written where they were.
UPDATE rl.t SET v = 'moved ' || id WHERE id BETWEEN 100 AND 200;
SELECT length(dolt_commit('-am', 'moved')::text) > 0 AS committed;
UPDATE rl.t SET v = 'again ' || id WHERE id BETWEEN 100 AND 200;
DELETE FROM rl.t WHERE id BETWEEN 300 AND 400;
VACUUM rl.t;
INSERT INTO rl.t SELECT g, 'new ' || g FROM generate_series(2000, 2100) g;
SELECT pg_temp.check() AS same_as_full;
SELECT count(*) AS changed FROM dolt_diff('HEAD', 'WORKING', NULL::rl.t);
SELECT length(dolt_commit('-am', 'reused')::text) > 0 AS committed;
SELECT count(*) AS committed_changes FROM dolt_diff('HEAD~1', 'HEAD', NULL::rl.t);
SELECT * FROM dolt_status;

\c :home
DROP DATABASE row_locations;
