-- A value stored out of line that only a retained frozen page root still
-- names (its commit's: the row updated since, the old version pruned, the
-- commit severed, its page root kept by root retention) survives a retire
-- of its generation, and a full collection: the roots' pages are read for
-- hash pointers as the tables' are.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE DATABASE generations_roots_db;
\c generations_roots_db
CREATE EXTENSION test_prolly;
CREATE SCHEMA fr VERSIONED;
CREATE TABLE fr.t (id int PRIMARY KEY, v text);
INSERT INTO fr.t VALUES (0, 'stays'), (1, (SELECT string_agg(md5('frozen' || i), '' ORDER BY i)
	FROM generate_series(1, 3000) i));
SELECT (test_toast_pointer(v)).root AS val FROM fr.t WHERE id = 1 \gset
SELECT length(dolt_commit('-Am', 'c1', '--date', '2020-01-01')::text) > 0 AS committed;
SELECT kept >= 0 AS collected FROM test_chunk_gc(false);
SELECT test_chunk_generation() AS gv \gset
UPDATE fr.t SET v = 'small' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'c2', '--date', '2020-01-02')::text) > 0 AS committed;
VACUUM fr.t;
SELECT kept >= 0 AS collected FROM test_chunk_gc(false);
UPDATE fr.t SET v = 'small2' WHERE id = 0;
SELECT length(dolt_commit('-Am', 'c3', '--date', '2020-01-03')::text) > 0 AS committed;
SELECT kept >= 0 AS collected FROM test_chunk_gc(false);
SELECT commits FROM dolt_history_drop('2020-01-02');

SELECT kept >= 0 AS retired FROM test_chunk_retire(:gv, :gv);
SELECT test_chunk_has(:'val') AS kept_after_retire;
SELECT chunks_freed >= 0 AS collected FROM prolly_gc(true);
SELECT test_chunk_has(:'val') AS kept_after_full;
SELECT test_chunk_verify_generations() > 0 AS generations;

\c postgres
DROP DATABASE generations_roots_db;
