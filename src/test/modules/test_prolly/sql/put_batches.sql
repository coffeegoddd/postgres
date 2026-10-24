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
-- (each session's counts flushed before it reconnects: a session's last
-- counts, flushed only as it exits, could reach the view after the next
-- session's reset, and be counted as that session's)

SELECT count(test_chunk_put(test_noise('in the journal ' || g, 500)))
FROM generate_series(1, 10) g;
SELECT test_chunk_table_file(array_agg(test_noise('in a file ' || g, 500)))
FROM generate_series(1, 10) g;

-- Ten the journal has, ten a table file has, five new, each put twice:
-- the five are appended, once each.
:flush
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
:flush
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

-- A savepoint after chunks the store has, a batch of them all dropped
-- (nothing appended), then a chunk put and rolled back to the savepoint:
-- the chunk is gone with it, and the commit appends nothing.
:flush
\c puts_db
SELECT journal_bytes() AS before \gset
BEGIN;
SELECT count(test_chunk_put(test_noise('big ' || g, 100000)))
FROM generate_series(1, 6) g;
SAVEPOINT a;
SELECT count(test_chunk_put(test_noise('big ' || g, 100000)))
FROM generate_series(7, 12) g;
SELECT test_chunk_put(test_noise('rolled back', 1000)) IS NOT NULL AS put;
ROLLBACK TO a;
COMMIT;
SELECT journal_bytes() = :before AS nothing_appended;
:flush
\c puts_db
SELECT test_chunk_has(test_content_hash(test_noise('rolled back', 1000))) AS rolled_back_found;
SELECT test_chunk_has(test_content_hash(test_noise('big 12', 100000))) AS big_found;
SELECT test_chunk_get(test_chunk_put(test_noise('after', 1000))) = test_noise('after', 1000)
	AS journal_reads;

\c postgres
DROP DATABASE puts_db;
