-- Compaction: a page with a fraction of its bytes dead (its chunks freed)
-- has its chunks' bytes put elsewhere, their entries moved; once VACUUM
-- takes the old entries out the page is free, and used again.
CREATE EXTENSION IF NOT EXISTS test_prolly;
SET chunk_free_in_background = off;
CREATE TABLE cl () USING prolly_chunks;
CREATE TABLE ci () USING prolly_chunk_index;
CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS 'SELECT substr(sha256(d), 1, 20)';
CREATE FUNCTION blob(tag text) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT convert_to(rpad(tag, 2000, '.'), 'UTF8')$$;

-- 400 chunks of 2000 bytes, about four a page, every other one held by a
-- root, and the others freed: every page about half dead (some a little
-- under: compacted at 0.4 here), none free
SELECT count(*) FILTER (WHERE test_chunk_put('cl', 'ci', h(blob('a' || g)), blob('a' || g))) AS put
FROM generate_series(1, 400) g;
SELECT count(*) FROM generate_series(2, 400, 2) g, LATERAL test_chunk_ref_root('ci', h(blob('a' || g)));
SELECT sum(test_chunk_free('cl', 'ci', h(blob('a' || g)))) AS freed FROM generate_series(1, 400, 2) g;
VACUUM ci;
SELECT pages, free_pages FROM test_chunk_log_space('cl');
SELECT pg_relation_size('cl') AS size_written \gset

-- nothing to compact if more must be dead
SELECT pages, chunks FROM test_chunk_compact('cl', 'ci', 0.9, 0);

-- compacted: the 200 kept moved, read back, the audit clean (the old
-- entries freed, their bytes the store's still until VACUUM)
SELECT pages > 90 AS most_pages, chunks, bytes FROM test_chunk_compact('cl', 'ci', 0.4, 0);
SELECT count(*) FILTER (WHERE test_chunk_get('cl', 'ci', h(blob('a' || g))) = blob('a' || g)) AS read_back
FROM generate_series(2, 400, 2) g;
SELECT ok, chunks FROM test_chunk_check('cl', 'ci');

-- their counts the same: each a root's still
SELECT count(*) FILTER (WHERE refs = 0 AND roots = 1) AS counts_kept
FROM generate_series(2, 400, 2) g, LATERAL test_chunk_index_entry('ci', h(blob('a' || g)));

-- VACUUM takes the old entries out: the pages compacted free, none left
-- to compact
VACUUM ci;
SELECT free_pages > 90 AS pages_compacted_free FROM test_chunk_log_space('cl');
SELECT pages, chunks FROM test_chunk_compact('cl', 'ci', 0.4, 0);
SELECT pg_relation_size('cl') AS size_compacted \gset

-- 200 more go into the pages freed: the file no bigger
\c
SET chunk_free_in_background = off;
SELECT count(*) FILTER (WHERE test_chunk_put('cl', 'ci', h(blob('b' || g)), blob('b' || g))) AS put_again
FROM generate_series(1, 200) g;
SELECT pg_relation_size('cl') = :size_compacted AS no_bigger;
SELECT count(*) FILTER (WHERE test_chunk_get('cl', 'ci', h(blob('a' || g))) = blob('a' || g)) AS kept_read
FROM generate_series(2, 400, 2) g;
SELECT count(*) FILTER (WHERE test_chunk_get('cl', 'ci', h(blob('b' || g))) = blob('b' || g)) AS new_read
FROM generate_series(1, 200) g;
SELECT ok, chunks FROM test_chunk_check('cl', 'ci');

-- room: a compaction stops once it has moved that many bytes (the pages
-- it looked at last done)
SELECT sum(test_chunk_free('cl', 'ci', h(blob('b' || g)))) AS freed FROM generate_series(1, 200, 2) g;
VACUUM ci;
SELECT chunks < 100 AS some, bytes >= 20000 AS room_moved FROM test_chunk_compact('cl', 'ci', 0.4, 20000);
SELECT ok FROM test_chunk_check('cl', 'ci');

DROP TABLE cl, ci;
DROP FUNCTION h(bytea), blob(text);
