-- Space back: a chunk's bytes are dead once its entry is out (freed, or
-- of a put rolled back), and a page all of whose bytes are dead is used
-- again for appends, so the log's file doesn't grow.
CREATE EXTENSION IF NOT EXISTS test_prolly;
CREATE TABLE ql () USING prolly_chunks;
CREATE TABLE qi () USING prolly_chunk_index;
CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS 'SELECT substr(sha256(d), 1, 20)';
CREATE FUNCTION blob(tag text) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT convert_to(rpad(tag, 2000, '.'), 'UTF8')$$;

-- 200 chunks of 2000 bytes, about four a page
SELECT count(*) FILTER (WHERE test_chunk_put('ql', 'qi', h(blob('a' || g)), blob('a' || g))) AS put
FROM generate_series(1, 200) g;
SELECT pages, full_pages, dead, free_pages FROM test_chunk_log_space('ql');
SELECT pg_relation_size('ql') AS size_full \gset

-- freed, then VACUUM takes their entries out: their bytes dead, the full
-- pages free
SELECT test_chunk_free_unheld('ql', 'qi') AS freed;
VACUUM qi;
SELECT pages, full_pages, dead = 200 * 2000 AS all_dead, free_pages = full_pages AS full_ones_free
FROM test_chunk_log_space('ql');

-- 150 more, from a backend with no region of its own (one with a region
-- fills it first), go into the pages freed: the file no bigger
\c
SELECT count(*) FILTER (WHERE test_chunk_put('ql', 'qi', h(blob('b' || g)), blob('b' || g))) AS put_again
FROM generate_series(1, 150) g;
SELECT pg_relation_size('ql') = :size_full AS no_bigger;
SELECT count(*) FILTER (WHERE test_chunk_get('ql', 'qi', h(blob('b' || g))) = blob('b' || g)) AS read_back
FROM generate_series(1, 150) g;
SELECT ok, chunks FROM test_chunk_check('ql', 'qi');

-- a put rolled back: its bytes dead once VACUUM takes its entry out
SELECT dead AS dead_before FROM test_chunk_log_space('ql') \gset
BEGIN;
SELECT test_chunk_put('ql', 'qi', h(blob('gone')), blob('gone')) AS put;
ROLLBACK;
VACUUM qi;
SELECT dead - :dead_before AS dead_added FROM test_chunk_log_space('ql');

DROP TABLE ql, qi;
DROP FUNCTION blob(text), h(bytea);
