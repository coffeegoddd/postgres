-- History severed after collections settled it in generations of its
-- own, freed by a retire of the generations settled before the sever
-- (whose files also hold the refs that named it then): the history past
-- the horizon is gone, the rest reads, and the generation settled since
-- is neither read nor rewritten.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
\set VERBOSITY terse
CREATE DATABASE generations_retire_sever_db;
\c generations_retire_sever_db
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
SELECT test_chunk_generation() AS g1 \gset
UPDATE gs.t SET v = 'd2' WHERE id % 3 = 0;
SELECT length(dolt_commit('-Am', 'd2', '--date', '2020-01-02')::text) > 0 AS committed;
SELECT kept >= 0 AS collected FROM test_chunk_gc(false);
SELECT test_chunk_generation() AS g2 \gset
UPDATE gs.t SET v = 'd3' WHERE id % 3 = 1;
SELECT length(dolt_commit('-Am', 'd3', '--date', '2020-01-03')::text) > 0 AS committed;
SELECT kept >= 0 AS collected FROM test_chunk_gc(false);
UPDATE gs.t SET v = 'd4' WHERE id % 3 = 2;
SELECT length(dolt_commit('-Am', 'd4', '--date', '2020-01-04')::text) > 0 AS committed;
SELECT kept >= 0 AS collected FROM test_chunk_gc(false);
SELECT count(*) AS settled_files FROM test_chunk_generations() WHERE settled;
SELECT count(*) AS d1_d2 FROM dolt_diff('main~3', 'main~2', NULL::gs.t);

SELECT test_chunk_generation() AS g4 \gset

SELECT commits FROM dolt_history_drop('2020-01-03');
UPDATE gs.t SET v = 'd5' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'd5', '--date', '2020-01-05')::text) > 0 AS committed;
SELECT kept >= 0 AS collected FROM test_chunk_gc(false);
SELECT name AS since FROM test_chunk_generations() WHERE epoch_lo > :g4 \gset
SELECT freed > 0 AS freed FROM test_chunk_retire(:g1, :g4);
SELECT count(*) AS since_untouched FROM test_chunk_generations()
	WHERE name = :'since';
SELECT gs_err($$SELECT count(*) FROM dolt_diff('main~4', 'main~3', NULL::gs.t)$$) AS d1_d2;
SELECT count(*) AS d3_d4 FROM dolt_diff('main~2', 'main~1', NULL::gs.t);
SELECT test_chunk_verify_generations() > 0 AS generations;

\c postgres
DROP DATABASE generations_retire_sever_db;
