-- Roots captured into a page store: a relation's tree as it was, held by
-- the store rather than the relation, so any number of them, outliving the
-- relation's storage.  Capturing one is a snapshot's map work; a relation
-- made of one has the pages it held; holding and letting go are
-- transactional.

-- A database of its own, so nothing else counts in it.
SELECT current_database() AS home \gset
CREATE DATABASE bmap_captured_roots;
\c bmap_captured_roots
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
SET debug_block_mapped_heap = on;

CREATE FUNCTION pg_temp.counted() RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$
	SELECT n, coalesce((SELECT p.value FROM pg_stat_prolly p WHERE p.name = n), -1)
	FROM unnest('{bmap_map_pages_copied,bmap_roots_captured,bmap_roots_released,bmap_roots_shared}'::text[]) n
$$;

CREATE TABLE small (a int, b text) WITH (fillfactor = 10);
INSERT INTO small SELECT g, repeat('s', 200) FROM generate_series(1, 3000) g;
CREATE TABLE big (a int, b text) WITH (fillfactor = 10);
INSERT INTO big SELECT g, repeat('b', 200) FROM generate_series(1, 30000) g;
SELECT pg_relation_filenode('small') AS small, pg_relation_filenode('big') AS big \gset
SELECT test_bmap_rows_digest(:big) AS big_rows \gset

-- capturing one: no map page copied, whatever the size
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_root_capture(:big) AS big_root \gset
SELECT pg_stat_force_next_flush();
SELECT * FROM pg_temp.counted();
SELECT test_bmap_root_held(:big_root) = pg_relation_size('big') / 8192 AS its_length;
SELECT * FROM test_bmap_roots();

-- more roots than a relation's root page keeps snapshots: all held
CREATE TEMP TABLE many AS
	SELECT g, test_bmap_root_capture(:small) AS id FROM generate_series(1, 100) g;
SELECT count(*) AS captured, count(DISTINCT id) AS distinct_ids,
	bool_and(test_bmap_root_held(id) IS NOT NULL) AS all_held FROM many;
SELECT held, slots >= held AS slots_cover FROM test_bmap_roots();

-- the relation changes: each page changed is copied, the root keeps it
UPDATE big SET b = 'changed' WHERE a % 100 = 0;
DELETE FROM big WHERE a > 25000;
SELECT test_bmap_rows_digest(:big) <> :'big_rows' AS big_changed;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_share_root(:big_root) AS then \gset
SELECT pg_stat_force_next_flush();
SELECT * FROM pg_temp.counted();
SELECT test_bmap_rows_digest(:then) = :'big_rows' AS then_as_it_was;
SELECT test_bmap_write(:then, 10, 1);
SELECT test_bmap_rows_digest(:then) = :'big_rows' AS then_written;
SELECT test_bmap_unlink(:then);

-- the relation dropped: its root outlives it, its pages with it
DROP TABLE big;
SELECT test_bmap_root_held(:big_root) IS NOT NULL AS held_after_drop;
-- (a sweep frees nothing the root holds; the store may shorten by what
-- the drop let go of)
SELECT freed FROM prolly_page_sweep();
SELECT test_bmap_share_root(:big_root) AS after \gset
SELECT test_bmap_rows_digest(:after) = :'big_rows' AS after_drop_as_it_was;
SELECT test_bmap_unlink(:after);

-- letting go is the transaction's: rolled back, the root is held still
BEGIN;
SELECT test_bmap_root_release(:big_root);
SELECT test_bmap_root_held(:big_root) IS NULL AS let_go_in_it;
ROLLBACK;
SELECT test_bmap_root_held(:big_root) IS NOT NULL AS held_after_rollback;
SELECT * FROM prolly_page_sweep();
SELECT test_bmap_share_root(:big_root) AS again \gset
SELECT test_bmap_rows_digest(:again) = :'big_rows' AS still_as_it_was;
SELECT test_bmap_unlink(:again);

-- committed, it is let go of, and once the release queue drains (a sweep
-- drains it first), what only it held is freed
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_root_release(:big_root);
SELECT pg_stat_force_next_flush();
SELECT * FROM pg_temp.counted();
SELECT test_bmap_root_held(:big_root) IS NULL AS let_go;
SELECT test_bmap_root_release(:big_root) AS released_twice;
SELECT pg_stat_reset_shared('prolly');
SELECT count(*) FROM prolly_page_sweep();
SELECT pg_stat_force_next_flush();
SELECT value > 0 AS freed FROM pg_stat_prolly
WHERE name = 'bmap_release_blocks_freed';

-- a capture whose transaction rolls back holds nothing, and its slot is
-- reclaimed
BEGIN;
SELECT test_bmap_root_capture(:small) AS rolled_back \gset
SELECT test_bmap_root_held(:rolled_back) IS NOT NULL AS held_in_it;
ROLLBACK;
SELECT test_bmap_root_held(:rolled_back) IS NULL AS not_held;
SELECT test_bmap_roots_reclaim();
SELECT test_bmap_roots_reclaim() AS reclaimed_twice;

-- slots let go of are reused: as many held as before, no more slots
SELECT held AS held_before, slots AS slots_before FROM test_bmap_roots() \gset
SELECT count(*) FILTER (WHERE test_bmap_root_release(id)) FROM many WHERE g <= 50;
SELECT count(*) FROM (SELECT test_bmap_root_capture(:small) FROM generate_series(1, 50)) c;
SELECT held = :held_before AS as_many_held, slots = :slots_before AS slots_reused
FROM test_bmap_roots();
SELECT * FROM prolly_page_sweep();

-- A capture after one page changed walks the own nodes of the path copied
-- for it, a node of every leaf of the relation among them: their counts are
-- read a count page at a time, not a page read for each child.
CREATE TABLE wide (a int, b text) WITH (fillfactor = 10);
INSERT INTO wide SELECT g, repeat('w', 200) FROM generate_series(1, 30000) g;
SELECT pg_relation_filenode('wide') AS wide \gset
SELECT count(*) FILTER (WHERE level = 0) > 16 AS many_leaves,
	max(level) AS levels
FROM test_bmap_tree(:wide);
SELECT test_bmap_root_capture(:wide) AS w1 \gset
UPDATE wide SET b = 'x' WHERE a = 29000;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_root_capture(:wide) AS w2 \gset
SELECT pg_stat_force_next_flush();
SELECT value BETWEEN 1 AND 6 AS counts_read_by_page
FROM pg_stat_prolly WHERE name = 'bmap_refcount_reads';
SELECT test_bmap_root_release(:w1) AND test_bmap_root_release(:w2) AS let_go;
DROP TABLE wide;
SELECT count(*) FROM prolly_page_sweep();

\c :home
DROP DATABASE bmap_captured_roots;
