-- Compaction: the log's space is handed out and taken back a segment at a
-- time (32 pages, aligned), and a segment with a fraction of its bytes
-- dead (its chunks freed) has its live chunks moved, packed, into another;
-- once VACUUM takes the old entries out it is free, and used again.  The
-- live chunks of the log's last segments are moved into the free segments
-- below them (evacuated), so that VACUUM can cut the end off.
CREATE EXTENSION IF NOT EXISTS test_prolly;
SET chunk_free_in_background = off;
CREATE TABLE cl () USING prolly_chunks;
CREATE TABLE ci () USING prolly_chunk_index;
CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS 'SELECT substr(sha256(d), 1, 20)';
CREATE FUNCTION blob(tag text) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT convert_to(rpad(tag, 2000, '.'), 'UTF8')$$;

-- 2,000 chunks of 2000 bytes (16 segments, the last part written), every
-- other one held by a root, and the others freed: every segment about half
-- dead (a little under: compacted at 0.4 here), none free
SELECT count(*) FILTER (WHERE test_chunk_put('cl', 'ci', h(blob('a' || g)), blob('a' || g))) AS put
FROM generate_series(1, 2000) g;
SELECT count(*) FROM generate_series(2, 2000, 2) g, LATERAL test_chunk_ref_root('ci', h(blob('a' || g)));
SELECT sum(test_chunk_free('cl', 'ci', h(blob('a' || g)))) AS freed FROM generate_series(1, 2000, 2) g;
VACUUM ci;
SELECT segments, free, compactable, worth, evacuable, grow FROM test_chunk_log_segments('cl', 0.4);
SELECT pg_relation_size('cl') AS size_written \gset

-- nothing to compact if more must be dead
SELECT segments, chunks FROM test_chunk_compact('cl', 'ci', 0.9, false);

-- compacted: the kept chunks on the 15 segments written full moved (into
-- segments new, none free), read back, their counts the same, the audit
-- clean (the old entries freed, their bytes the store's still until VACUUM)
SELECT segments, chunks, bytes FROM test_chunk_compact('cl', 'ci', 0.4, false);
SELECT count(*) FILTER (WHERE test_chunk_get('cl', 'ci', h(blob('a' || g))) = blob('a' || g)) AS read_back
FROM generate_series(2, 2000, 2) g;
SELECT count(*) FILTER (WHERE refs = 0 AND roots = 1) AS counts_kept
FROM generate_series(2, 2000, 2) g, LATERAL test_chunk_index_entry('ci', h(blob('a' || g)));
SELECT ok, chunks FROM test_chunk_check('cl', 'ci');

-- VACUUM takes the old entries out: the 15 segments compacted free, none
-- left to compact; the end (the chunks moved) not to evacuate while this
-- backend's region, its last segment, may be appended to still
VACUUM ci;
SELECT segments, free, compactable, worth, evacuable FROM test_chunk_log_segments('cl', 0.4);

-- once it exited, and VACUUM let its region go, the end is to evacuate:
-- the chunks there moved into the free segments below; VACUUM then cuts
-- the end off, the log smaller than it was written, and every chunk there
\c
SET chunk_free_in_background = off;
VACUUM ci;
SELECT segments, free, compactable, worth, evacuable FROM test_chunk_log_segments('cl', 0.4);
SELECT segments, chunks FROM test_chunk_compact('cl', 'ci', 0.4, true);
VACUUM ci;
SELECT pg_relation_size('cl') < :size_written * 0.6 AS cut_to_live;
SELECT pg_relation_size('cl') AS size_compacted \gset
SELECT count(*) FILTER (WHERE test_chunk_get('cl', 'ci', h(blob('a' || g))) = blob('a' || g)) AS read_back
FROM generate_series(2, 2000, 2) g;
SELECT ok, chunks FROM test_chunk_check('cl', 'ci');

-- 100 more go into the free segment left (the log cut to a segment more
-- than its live chunks need): the file no bigger
\c
SET chunk_free_in_background = off;
SELECT segments, free FROM test_chunk_log_segments('cl', 0.4);
SELECT count(*) FILTER (WHERE test_chunk_put('cl', 'ci', h(blob('b' || g)), blob('b' || g))) AS put_again
FROM generate_series(1, 100) g;
SELECT pg_relation_size('cl') = :size_compacted AS no_bigger;
SELECT count(*) FILTER (WHERE test_chunk_get('cl', 'ci', h(blob('b' || g))) = blob('b' || g)) AS new_read
FROM generate_series(1, 100) g;
SELECT ok, chunks FROM test_chunk_check('cl', 'ci');

DROP TABLE cl, ci;
DROP FUNCTION h(bytea), blob(text);
