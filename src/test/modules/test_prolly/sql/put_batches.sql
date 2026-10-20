-- Whether the store has a chunk put is asked of every pending chunk at
-- once, as they are appended (a batch, or the rest at commit), not as each
-- is put: those it has, in the journal or a table file, are dropped then,
-- and only the rest are appended and counted (chunks_appended).
\c postgres
CREATE DATABASE puts_db;
\c puts_db
CREATE EXTENSION test_prolly;
CREATE FUNCTION journal_bytes() RETURNS bigint LANGUAGE sql AS $$
	SELECT sum(bytes)::bigint FROM test_chunk_segments()
$$;
CREATE FUNCTION counted(name text) RETURNS bigint LANGUAGE sql AS $$
	SELECT value FROM pg_stat_prolly WHERE pg_stat_prolly.name = counted.name
$$;
\set reset 'SELECT pg_stat_force_next_flush(); SELECT pg_stat_reset_shared(''prolly'');'
\set flush 'SELECT pg_stat_force_next_flush();'

SELECT count(test_chunk_put(test_noise('in the journal ' || g, 500)))
FROM generate_series(1, 10) g;
SELECT test_chunk_table_file(array_agg(test_noise('in a file ' || g, 500)))
FROM generate_series(1, 10) g;

-- Ten the journal has, ten a table file has, five new, each put twice:
-- the five are appended, once each.
\c puts_db
SELECT journal_bytes() AS before \gset
:reset
BEGIN;
SELECT count(test_chunk_put(test_noise(kind || g, 500)))
FROM (VALUES ('in the journal '), ('in a file '), ('new ')) k(kind),
	generate_series(1, 10) g, generate_series(1, 2) twice
WHERE kind <> 'new ' OR g <= 5;
COMMIT;
:flush
SELECT counted('chunks_appended') AS appended,
	counted('chunk_bytes_appended') AS bytes_appended,
	journal_bytes() - :before BETWEEN 2500 AND 2800 AS journal_grew_by_five;
SELECT count(*) AS all_found FROM (VALUES ('in the journal '), ('in a file '), ('new ')) k(kind),
	generate_series(1, 10) g
WHERE (kind <> 'new ' OR g <= 5) AND test_chunk_has(test_content_hash(test_noise(kind || g, 500)));

-- A batch past a megabyte, of chunks the store has but for one: only
-- that one appended, before the commit.
SELECT count(test_chunk_put(test_noise('big ' || g, 100000)))
FROM generate_series(1, 12) g;
\c puts_db
SELECT journal_bytes() AS before \gset
:reset
BEGIN;
SELECT test_chunk_put(test_noise('one more', 100000)) IS NOT NULL AS put;
SELECT count(test_chunk_put(test_noise('big ' || g, 100000)))
FROM generate_series(1, 12) g;
SELECT journal_bytes() - :before BETWEEN 100000 AND 100200 AS one_appended;
COMMIT;
:flush
SELECT counted('chunks_appended') AS appended;

\c postgres
DROP DATABASE puts_db;
