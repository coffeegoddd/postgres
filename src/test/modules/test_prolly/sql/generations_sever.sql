-- History severed after collections settled it in generations of its
-- own: a full collection frees it, as it does when nothing was settled.
-- (A generation's file is small, and is rewritten whatever its garbage:
-- kept whole, it would keep the severed history it holds.)
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
\set VERBOSITY terse
CREATE DATABASE generations_sever_db;
\c generations_sever_db
CREATE EXTENSION test_prolly;
CREATE FUNCTION gs_err(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN regexp_replace(SQLERRM, '[0-9a-f]{40}', '<hash>', 'g');
END $$;
CREATE SCHEMA gs VERSIONED;
CREATE TABLE gs.t (id int PRIMARY KEY, v text);
INSERT INTO gs.t SELECT g, 'v' || g FROM generate_series(1, 300) g;
SELECT length(dolt_commit('-Am', 'd1', '--date', '2020-01-01')::text) > 0 AS committed;
SELECT kept >= 0 AS collected FROM test_chunk_gc(false);
UPDATE gs.t SET v = 'd2' WHERE id % 3 = 0;
SELECT length(dolt_commit('-Am', 'd2', '--date', '2020-01-02')::text) > 0 AS committed;
SELECT kept >= 0 AS collected FROM test_chunk_gc(false);
UPDATE gs.t SET v = 'd3' WHERE id % 3 = 1;
SELECT length(dolt_commit('-Am', 'd3', '--date', '2020-01-03')::text) > 0 AS committed;
SELECT kept >= 0 AS collected FROM test_chunk_gc(false);
UPDATE gs.t SET v = 'd4' WHERE id % 3 = 2;
SELECT length(dolt_commit('-Am', 'd4', '--date', '2020-01-04')::text) > 0 AS committed;
SELECT kept >= 0 AS collected FROM test_chunk_gc(false);
SELECT count(*) AS settled_files FROM test_chunk_generations() WHERE settled;
SELECT count(*) AS d1_d2 FROM dolt_diff('main~3', 'main~2', NULL::gs.t);

SELECT commits FROM dolt_history_drop('2020-01-03');
SELECT chunks_freed > 0 AS freed FROM prolly_gc(true);
SELECT gs_err($$SELECT count(*) FROM dolt_diff('main~3', 'main~2', NULL::gs.t)$$) AS d1_d2;
SELECT count(*) AS d3_d4 FROM dolt_diff('main~1', 'main', NULL::gs.t);
SELECT test_chunk_verify_generations() > 0 AS generations;

\c postgres
DROP DATABASE generations_sever_db;
