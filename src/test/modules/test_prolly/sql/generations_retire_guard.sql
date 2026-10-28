-- A repair settles a severed commit's data only in a generation the
-- commit's file can name it from: as new as its own, or listed by it.  A
-- retire relies on it (it reads only the sections of the files it doesn't
-- collect, and every chunk of the files a repair wrote).  Here the older
-- of two fragments is repaired, on purpose, into a generation newer than
-- the files of the commits naming its data: refused.
\getenv abs_builddir PG_ABS_BUILDDIR
\set dir :abs_builddir '/tmp_check/generations_retire_guard'
SELECT 'file://' || :'dir' || '/' || txid_current() AS url \gset
\set VERBOSITY terse
SELECT current_database() AS home \gset
CREATE DATABASE regress_generations_guard;
\c regress_generations_guard
CREATE EXTENSION test_prolly;
CREATE SCHEMA gr VERSIONED;
CREATE TABLE gr.t (id int PRIMARY KEY, v text);
CREATE FUNCTION gr_commit(msg text, day int) RETURNS text LANGUAGE sql AS $$
	SELECT trim(both '{}' from dolt_commit('-Am', msg, '--date',
		(timestamptz '2020-01-01 00:00+00' + make_interval(days => day))::text)::text)
$$;
INSERT INTO gr.t SELECT g, 'v' || g FROM generate_series(1, 300) g;
SELECT length(gr_commit('d1', 1)) AS committed;
UPDATE gr.t SET v = 'd2' WHERE id % 4 = 0;
SELECT length(gr_commit('d2', 2)) AS committed;
UPDATE gr.t SET v = 'd3' WHERE id % 4 = 1;
SELECT length(gr_commit('d3', 3)) AS committed;
UPDATE gr.t SET v = 'd4' WHERE id % 4 = 2;
SELECT length(gr_commit('d4', 4)) AS committed;
SELECT dolt_remote('add', 'origin', :'url') IS NOT NULL AS added;
SELECT dolt_push('origin', 'main') IS NOT NULL AS pushed;
SELECT chunks_freed >= 0 AS collected FROM prolly_gc(false);
UPDATE gr.t SET v = 'd5' WHERE id % 4 = 3;
SELECT length(gr_commit('d5', 5)) AS committed;
UPDATE gr.t SET v = 'd6' WHERE id % 5 = 0;
SELECT length(gr_commit('d6', 6)) AS committed;
SELECT dolt_push('origin', 'main') IS NOT NULL AS pushed;
SELECT commits FROM dolt_history_drop('2020-01-03');
SELECT chunks_freed >= 0 AS collected FROM prolly_gc(true);
SELECT commits FROM dolt_history_drop('2020-01-05');
SELECT chunks_freed >= 0 AS collected FROM prolly_gc(true);
SELECT dolt_history_repair('origin', 1) AS repaired;

-- The older fragment repaired into a new generation, as a wrong settle
-- would: refused, nothing settled, as the commit naming its data is
-- older.  (The check of generations off, as outside assert builds.)
SET debug_prolly_verify_generations = off;
SELECT test_chunk_generation() + 5 AS wrong \gset
SET debug_prolly_settle_epoch = :wrong;
-- (the chunks named by hash, not shown: the commit's depends on the name
-- of the role the test runs as)
DO $$
BEGIN
	PERFORM dolt_history_repair('origin');
	RAISE NOTICE 'repaired';
EXCEPTION WHEN OTHERS THEN
	RAISE NOTICE 'refused: %', regexp_replace(SQLERRM, '[0-9a-f]{40}', '<hash>', 'g');
END
$$;
RESET debug_prolly_settle_epoch;
SELECT count(*) AS fragments FROM prolly_history_horizon();
SELECT kept >= 0 AS retired FROM test_chunk_retire(:wrong, :wrong);
SELECT count(*) AS commits FROM dolt_log('main');

-- Then repaired as it should be, and its generations retired, and every
-- generation since: the history stays whole.
SELECT dolt_history_repair('origin') AS repaired;
SELECT kept >= 0 AS retired
	FROM test_chunk_retire(1, test_chunk_generation());
SELECT count(*) AS commits FROM dolt_log('main');
SELECT count(*) AS d1_d2 FROM dolt_diff('main~5', 'main~4', NULL::gr.t);
SELECT test_chunk_verify_generations() > 0 AS generations;

\c :home
DROP DATABASE regress_generations_guard;
