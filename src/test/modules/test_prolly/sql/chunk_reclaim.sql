-- Space back: a chunk's bytes are dead once its entry is out (freed, or
-- of a put rolled back), and a page all of whose bytes are dead is used
-- again for appends, so the log's file doesn't grow.
CREATE EXTENSION IF NOT EXISTS test_prolly;
-- (chunks nothing holds kept across VACUUM: no freer in the background)
SET chunk_free_in_background = off;
CREATE TABLE ql () USING prolly_chunks;
CREATE TABLE qi () USING prolly_chunk_index;
CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS 'SELECT substr(sha256(d), 1, 20)';
CREATE FUNCTION blob(tag text) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT convert_to(rpad(tag, 2000, '.'), 'UTF8')$$;

-- 400 chunks of 2000 bytes, about four a page: three segments and a little
-- (the log's space is used again a segment at a time, every page of it free)
SELECT count(*) FILTER (WHERE test_chunk_put('ql', 'qi', h(blob('a' || g)), blob('a' || g))) AS put
FROM generate_series(1, 400) g;
-- and one with a root, at the log's end, so that VACUUM doesn't cut the
-- freed pages off (see below), and they are used again
SELECT test_chunk_put('ql', 'qi', h(blob('anchor')), blob('anchor')) AS anchor;
SELECT roots FROM test_chunk_ref_root('qi', h(blob('anchor')));
SELECT pages, full_pages, dead, free_pages FROM test_chunk_log_space('ql');
SELECT pg_relation_size('ql') AS size_full \gset

-- freed, then VACUUM takes their entries out: their bytes dead, the full
-- pages free (the three segments before the anchor's)
SELECT test_chunk_free_unheld('ql', 'qi') AS freed;
VACUUM qi;
SELECT pages, full_pages, dead >= 400 * 2000 AS all_dead, free_pages = full_pages AS full_ones_free
FROM test_chunk_log_space('ql');

-- 300 more, from a backend with no region of its own (one with a region
-- fills it first), go into the segments freed: the file no bigger
\c
SET chunk_free_in_background = off;
SELECT count(*) FILTER (WHERE test_chunk_put('ql', 'qi', h(blob('b' || g)), blob('b' || g))) AS put_again
FROM generate_series(1, 300) g;
SELECT pg_relation_size('ql') = :size_full AS no_bigger;
SELECT count(*) FILTER (WHERE test_chunk_get('ql', 'qi', h(blob('b' || g))) = blob('b' || g)) AS read_back
FROM generate_series(1, 300) g;
SELECT ok, chunks FROM test_chunk_check('ql', 'qi');

-- and again: pages used again are freed again (a page a region leaves
-- part written is made full, its rest dead), and used a third time: 250,
-- in the two segments wholly free (the third is the region of the backend
-- that put them, which VACUUM ran in).  The first backend's last page, the
-- anchor's, made full too, once it exited: every full page free but that
-- one
SELECT test_chunk_free_unheld('ql', 'qi') AS freed;
VACUUM qi;
SELECT full_pages - free_pages AS full_not_free FROM test_chunk_log_space('ql');
\c
SET chunk_free_in_background = off;
SELECT count(*) FILTER (WHERE test_chunk_put('ql', 'qi', h(blob('c' || g)), blob('c' || g))) AS put_third
FROM generate_series(1, 250) g;
SELECT pg_relation_size('ql') = :size_full AS still_no_bigger;
SELECT ok, chunks FROM test_chunk_check('ql', 'qi');

-- the anchor let go of, everything freed: VACUUM cuts the free tail off,
-- the whole log but its meta page; not with TRUNCATE off, nor with
-- vacuum_truncate off (a cut's lock conflicts with a hot standby's readers)
SELECT roots FROM test_chunk_unref_root('qi', h(blob('anchor')));
SELECT test_chunk_free_unheld('ql', 'qi') AS freed;
SELECT pg_relation_size('ql') AS before_cut \gset
VACUUM (TRUNCATE off) qi;
SELECT pg_relation_size('ql') = :before_cut AS not_cut_truncate_off;
SET vacuum_truncate = off;
VACUUM qi;
SELECT pg_relation_size('ql') = :before_cut AS not_cut_vacuum_truncate_off;
RESET vacuum_truncate;
-- (nor with the log's vacuum_truncate off, or the index's)
ALTER TABLE ql SET (vacuum_truncate = off);
VACUUM qi;
SELECT pg_relation_size('ql') = :before_cut AS not_cut_log_off;
ALTER TABLE ql RESET (vacuum_truncate);
ALTER TABLE qi SET (vacuum_truncate = off);
VACUUM qi;
SELECT pg_relation_size('ql') = :before_cut AS not_cut_index_off;
ALTER TABLE qi RESET (vacuum_truncate);
VACUUM qi;
SELECT pages, pg_relation_size('ql') = current_setting('block_size')::int AS meta_page_only
FROM test_chunk_log_space('ql');
SELECT count(*) FILTER (WHERE test_chunk_put('ql', 'qi', h(blob('d' || g)), blob('d' || g))) AS put_after_cut
FROM generate_series(1, 20) g;
SELECT ok, chunks FROM test_chunk_check('ql', 'qi');

-- a put rolled back: its bytes dead once VACUUM takes its entry out (a
-- log of its own, with no page free: a page used again starts with none
-- dead)
CREATE TABLE ql2 () USING prolly_chunks;
CREATE TABLE qi2 () USING prolly_chunk_index;
SELECT test_chunk_put('ql2', 'qi2', h(blob('kept')), blob('kept')) AS put;
BEGIN;
SELECT test_chunk_put('ql2', 'qi2', h(blob('gone')), blob('gone')) AS put;
ROLLBACK;
VACUUM qi2;
SELECT dead AS dead_after_rollback FROM test_chunk_log_space('ql2');
-- and so when it's looked up, and put again, before VACUUM
BEGIN;
SELECT test_chunk_put('ql2', 'qi2', h(blob('again')), blob('again')) AS put;
ROLLBACK;
SELECT test_chunk_has('ql2', 'qi2', h(blob('again'))) AS has_after_rollback;
SELECT test_chunk_put('ql2', 'qi2', h(blob('again')), blob('again')) AS put_again;
VACUUM qi2;
SELECT dead AS dead_after_second FROM test_chunk_log_space('ql2');
DROP TABLE ql2, qi2;

-- the log truncated alone is another log's storage: the index refuses it,
-- and VACUUM counts nothing dead in it
TRUNCATE ql;
SELECT test_chunk_put('ql', 'qi', h(blob('new')), blob('new'));
VACUUM qi;

DROP TABLE ql, qi;

-- chunks longer than a page, freed, and others as long put again: into
-- runs of the pages freed, the file no bigger
CREATE TABLE ql3 () USING prolly_chunks;
CREATE TABLE qi3 () USING prolly_chunk_index;
CREATE FUNCTION big(tag text) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT convert_to(rpad(tag, 20000, '.'), 'UTF8')$$;
SELECT count(*) FILTER (WHERE test_chunk_put('ql3', 'qi3', h(big('a' || g)), big('a' || g))) AS put
FROM generate_series(1, 40) g;
SELECT test_chunk_put('ql3', 'qi3', h(big('anchor')), big('anchor')) AS anchor;
SELECT roots FROM test_chunk_ref_root('qi3', h(big('anchor')));
SELECT pg_relation_size('ql3') AS size3 \gset
SELECT test_chunk_free_unheld('ql3', 'qi3') AS freed;
VACUUM qi3;
SELECT free_pages > 0 AS some_free FROM test_chunk_log_space('ql3');
\c
SET chunk_free_in_background = off;
SELECT count(*) FILTER (WHERE test_chunk_put('ql3', 'qi3', h(big('b' || g)), big('b' || g))) AS put_again
FROM generate_series(1, 30) g;
SELECT pg_relation_size('ql3') = :size3 AS no_bigger;
SELECT count(*) FILTER (WHERE test_chunk_get('ql3', 'qi3', h(big('b' || g))) = big('b' || g)) AS read_back
FROM generate_series(1, 30) g;
SELECT ok, chunks FROM test_chunk_check('ql3', 'qi3');
DROP TABLE ql3, qi3;
DROP FUNCTION big(text);

-- chunks of half a page and a little more, freed, and others as long put
-- again: into the segments freed, on across their pages (not a page at a
-- time, which would leave each page's rest, too short for the next, dead),
-- the file no bigger
CREATE TABLE ql4 () USING prolly_chunks;
CREATE TABLE qi4 () USING prolly_chunk_index;
CREATE FUNCTION mid(tag text) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT convert_to(rpad(tag, 4100, '.'), 'UTF8')$$;
SELECT count(*) FILTER (WHERE test_chunk_put('ql4', 'qi4', h(mid('a' || g)), mid('a' || g))) AS put
FROM generate_series(1, 240) g;
SELECT test_chunk_put('ql4', 'qi4', h(mid('anchor')), mid('anchor')) AS anchor;
SELECT roots FROM test_chunk_ref_root('qi4', h(mid('anchor')));
SELECT pg_relation_size('ql4') AS size4 \gset
SELECT test_chunk_free_unheld('ql4', 'qi4') AS freed;
VACUUM qi4;
\c
SET chunk_free_in_background = off;
SELECT count(*) FILTER (WHERE test_chunk_put('ql4', 'qi4', h(mid('b' || g)), mid('b' || g))) AS put_again
FROM generate_series(1, 180) g;
SELECT pg_relation_size('ql4') = :size4 AS no_bigger_run;
SELECT ok, chunks FROM test_chunk_check('ql4', 'qi4');
DROP TABLE ql4, qi4;
DROP FUNCTION mid(text);
DROP FUNCTION blob(text), h(bytea);
