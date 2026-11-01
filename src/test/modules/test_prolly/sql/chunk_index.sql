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

-- a chunk is put by its own name only
SELECT test_chunk_put('cl', 'ci', h('\x646566'), '\x616263');
SELECT test_chunk_has('cl', 'ci', h('\x646566')) AS has_misnamed;

-- its counts, changed in place, never below zero
SELECT * FROM test_chunk_index_adjust('ci', h('\x616263'), 2, 1);
SELECT * FROM test_chunk_index_adjust('ci', h('\x616263'), -1, 0);
SELECT * FROM test_chunk_index_adjust('ci', h('\x616263'), -2, 0);
SELECT * FROM test_chunk_index_adjust('ci', h('\x646566'), 1, 0);
SELECT refs, roots FROM test_chunk_index_entry('ci', h('\x616263'));
-- (a change gives its transaction an id, so that its commit flushes it)
BEGIN;
SELECT refs FROM test_chunk_index_adjust('ci', h('\x616263'), 0, 0);
SELECT txid_current_if_assigned() IS NOT NULL AS has_id;
COMMIT;
BEGIN;
SELECT test_chunk_put('cl', 'ci', h('\x616263'), '\x616263') AS found_there;
SELECT txid_current_if_assigned() IS NOT NULL AS has_id;
COMMIT;

-- removed
SELECT test_chunk_index_remove('ci', h('\x616263')) AS removed;
SELECT test_chunk_index_remove('ci', h('\x616263')) AS removed_again;
SELECT test_chunk_has('cl', 'ci', h('\x616263')) AS has_after;

-- 50,000 chunks: the index grows a bucket at a time, and every chunk is
-- found, and none that wasn't put
SELECT count(*) FILTER (WHERE test_chunk_put('cl', 'ci', h(d), d)) AS stored
FROM (SELECT convert_to('chunk ' || g, 'UTF8') AS d FROM generate_series(1, 50000) g) s;
SELECT buckets > 200 AS grew, pages < buckets * 2 AS chains_short,
	entries AS on_pages
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

-- an aborted transaction's chunk is nobody's: found by it, then by none,
-- and put again by another
BEGIN;
SELECT test_chunk_put('cl', 'ci', h('\x0a'), '\x0a') AS stored;
SELECT test_chunk_has('cl', 'ci', h('\x0a')) AS has_own;
ROLLBACK;
SELECT test_chunk_has('cl', 'ci', h('\x0a')) AS has_after_abort;
SELECT test_chunk_index_adjust('ci', h('\x0a'), 1, 0) IS NULL AS no_counts;
SELECT test_chunk_put('cl', 'ci', h('\x0a'), '\x0a') AS stored_again,
	test_chunk_get('cl', 'ci', h('\x0a')) AS got;
-- and a subtransaction's
BEGIN;
SAVEPOINT s;
SELECT test_chunk_put('cl', 'ci', h('\x0b'), '\x0b') AS stored;
ROLLBACK TO s;
SELECT test_chunk_has('cl', 'ci', h('\x0b')) AS has_after_rollback_to;
COMMIT;

-- VACUUM takes aborted transactions' entries out, notes the rest
-- committed, and advances the index's relfrozenxid
CREATE TABLE ci3 () USING prolly_chunk_index;
SELECT relfrozenxid <> '0'::xid AS has_frozenxid FROM pg_class WHERE relname = 'ci3';
BEGIN;
SELECT count(test_chunk_put('cl', 'ci3', h(d), d)) AS aborted
FROM (SELECT convert_to('gone ' || g, 'UTF8') AS d FROM generate_series(1, 300) g) s;
ROLLBACK;
SELECT count(test_chunk_put('cl', 'ci3', h(d), d)) AS committed
FROM (SELECT convert_to('kept ' || g, 'UTF8') AS d FROM generate_series(1, 300) g) s;
SELECT entries AS before_vacuum FROM test_chunk_index_shape('ci3');
-- (lookups note commits on entries, but freeze none: only VACUUM does)
SELECT count(*) FILTER (WHERE test_chunk_has('cl', 'ci3', h(convert_to('kept ' || g, 'UTF8')))) AS looked_up
FROM generate_series(1, 300) g;
SELECT test_chunk_index_unfrozen('ci3') AS unfrozen_before;
SELECT age(relfrozenxid) AS age_before FROM pg_class WHERE relname = 'ci3' \gset
SELECT txid_current() IS NOT NULL AS advanced_xid;
VACUUM ci3;
SELECT entries AS after_vacuum, test_chunk_index_unfrozen('ci3') AS unfrozen_after
FROM test_chunk_index_shape('ci3');
SELECT age(relfrozenxid) < :age_before AS frozenxid_advanced FROM pg_class WHERE relname = 'ci3';
SELECT count(*) FILTER (WHERE test_chunk_has('cl', 'ci3', h(convert_to('kept ' || g, 'UTF8')))) AS kept,
	count(*) FILTER (WHERE test_chunk_has('cl', 'ci3', h(convert_to('gone ' || g, 'UTF8')))) AS gone
FROM generate_series(1, 300) g;
DROP TABLE ci3;

-- truncated, it starts again
TRUNCATE ci;
SELECT * FROM test_chunk_index_shape('ci');
SELECT test_chunk_has('cl', 'ci', h('\x616263')) AS has_after_truncate;
-- and truncated in the transaction that made it
BEGIN;
CREATE TABLE ci_once () USING prolly_chunk_index;
SELECT test_chunk_put('cl', 'ci_once', h('\x01'), '\x01') AS stored;
TRUNCATE ci_once;
SELECT * FROM test_chunk_index_shape('ci_once');
SELECT test_chunk_put('cl', 'ci_once', h('\x01'), '\x01') AS stored_again;
COMMIT;
DROP TABLE ci_once;

-- no tuples, no rewrites
INSERT INTO ci DEFAULT VALUES;
ALTER TABLE ci SET ACCESS METHOD heap;
SELECT test_chunk_index_shape('cl');

DROP TABLE cl, ci;
DROP FUNCTION h(bytea);
