-- The history store's index: a chunk's hash to its place in the log and its
-- counts, by linear hashing in pages of its own; chunks put, got and found
-- over a log and its index.
CREATE EXTENSION IF NOT EXISTS test_prolly;
CREATE TABLE cl () USING prolly_chunks;
CREATE TABLE ci () USING prolly_chunk_index;
CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS 'SELECT substr(sha256(d), 1, 20)';

-- a new index: its meta page, directory and first buckets
SELECT * FROM test_chunk_index_shape('ci');

-- one chunk, put once
SELECT test_chunk_put('cl', 'ci', h('\x616263'), '\x616263') AS stored;
SELECT test_chunk_put('cl', 'ci', h('\x616263'), '\x616263') AS stored_again;
SELECT test_chunk_has('cl', 'ci', h('\x616263')) AS has,
	test_chunk_get('cl', 'ci', h('\x616263')) AS got,
	test_chunk_has('cl', 'ci', h('\x646566')) AS has_other,
	test_chunk_get('cl', 'ci', h('\x646566')) IS NULL AS got_other_null;
SELECT len, refs, roots FROM test_chunk_index_entry('ci', h('\x616263'));

-- its counts, changed in place, never below zero
SELECT * FROM test_chunk_index_adjust('ci', h('\x616263'), 2, 1);
SELECT * FROM test_chunk_index_adjust('ci', h('\x616263'), -1, 0);
SELECT * FROM test_chunk_index_adjust('ci', h('\x616263'), -2, 0);
SELECT * FROM test_chunk_index_adjust('ci', h('\x646566'), 1, 0);
SELECT refs, roots FROM test_chunk_index_entry('ci', h('\x616263'));

-- removed
SELECT test_chunk_index_remove('ci', h('\x616263')) AS removed;
SELECT test_chunk_index_remove('ci', h('\x616263')) AS removed_again;
SELECT test_chunk_has('cl', 'ci', h('\x616263')) AS has_after;

-- 50,000 chunks: the index grows a bucket at a time, and every chunk is
-- found, and none that wasn't put
SELECT count(*) FILTER (WHERE test_chunk_put('cl', 'ci', h(d), d)) AS stored
FROM (SELECT convert_to('chunk ' || g, 'UTF8') AS d FROM generate_series(1, 50000) g) s;
SELECT buckets > 200 AS grew, pages < buckets * 2 AS chains_short
FROM test_chunk_index_shape('ci');
SELECT count(*) FILTER (WHERE test_chunk_get('cl', 'ci', h(d)) = d) AS found
FROM (SELECT convert_to('chunk ' || g, 'UTF8') AS d FROM generate_series(1, 50000) g) s;
SELECT count(*) FILTER (WHERE test_chunk_has('cl', 'ci', h(d))) AS found_unput
FROM (SELECT convert_to('none ' || g, 'UTF8') AS d FROM generate_series(1, 1000) g) s;

-- half removed: the rest found, the half not, and put again
SELECT count(*) FILTER (WHERE test_chunk_index_remove('ci', h(d))) AS removed
FROM (SELECT convert_to('chunk ' || g, 'UTF8') AS d FROM generate_series(1, 50000, 2) g) s;
SELECT count(*) FILTER (WHERE test_chunk_has('cl', 'ci', h(d))) AS found,
	count(*) FILTER (WHERE g % 2 = 0 AND test_chunk_has('cl', 'ci', h(d))) AS found_kept
FROM (SELECT g, convert_to('chunk ' || g, 'UTF8') AS d FROM generate_series(1, 50000) g) s;
SELECT count(*) FILTER (WHERE test_chunk_put('cl', 'ci', h(d), d)) AS stored_again
FROM (SELECT convert_to('chunk ' || g, 'UTF8') AS d FROM generate_series(1, 50000, 2) g) s;

-- truncated, it starts again
TRUNCATE ci;
SELECT * FROM test_chunk_index_shape('ci');
SELECT test_chunk_has('cl', 'ci', h('\x616263')) AS has_after_truncate;

-- no tuples, no rewrites
INSERT INTO ci DEFAULT VALUES;
ALTER TABLE ci SET ACCESS METHOD heap;
SELECT test_chunk_index_shape('cl');

DROP TABLE cl, ci;
DROP FUNCTION h(bytea);
