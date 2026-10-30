-- Roots captured into a page store: a relation's tree as it was, held by
-- the store rather than the relation, so any number of them, outliving the
-- relation's storage.  Capturing one is a snapshot's map work; a relation
-- made of one has the pages it held; holding and letting go are
-- transactional.
--
-- The work is measured by the map nodes a step makes (test_bmap_tree()'s
-- blocks), by the WAL it writes and by the buffers it reads, each of which
-- would grow with the relation if the step visited each of its blocks.

-- A database of its own, so nothing else changes its page store.
SELECT current_database() AS home \gset
CREATE DATABASE bmap_captured_roots;
\c bmap_captured_roots
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
SET debug_block_mapped_heap = on;

-- The WAL this backend wrote since a mark, in bytes, leaving out full-page
-- images: neither another backend's WAL nor the first image of a page after
-- a checkpoint can tip a comparison of two steps' map work.
CREATE FUNCTION pg_temp.wal_since(mark int8) RETURNS int8
LANGUAGE sql AS $$ SELECT test_bmap_own_wal() - mark $$;

-- The shared buffers a statement reads, hit or not.
CREATE FUNCTION pg_temp.buffers_read(query text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	plan json;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) ' || query INTO plan;
	RETURN (plan->0->'Plan'->>'Shared Hit Blocks')::bigint +
		(plan->0->'Plan'->>'Shared Read Blocks')::bigint;
END $$;

-- Autovacuum stays off the tables: a VACUUM would change their pages, and
-- write WAL, while the work of a step is measured.
CREATE TABLE small (a int, b text) WITH (fillfactor = 10, autovacuum_enabled = off);
INSERT INTO small SELECT g, repeat('s', 200) FROM generate_series(1, 3000) g;
CREATE TABLE big (a int, b text) WITH (fillfactor = 10, autovacuum_enabled = off);
INSERT INTO big SELECT g, repeat('b', 200) FROM generate_series(1, 30000) g;
SELECT pg_relation_filenode('small') AS small, pg_relation_filenode('big') AS big \gset
SELECT test_bmap_rows_digest(:big) AS big_rows \gset

-- Capturing one copies no map node of the relation's, whatever its size.
CREATE TEMP TABLE big_nodes AS SELECT block FROM test_bmap_tree(:big);
SELECT test_bmap_root_capture(:big) AS big_root \gset
SELECT count(*) AS nodes_copied FROM test_bmap_tree(:big)
WHERE block NOT IN (SELECT block FROM big_nodes);
SELECT test_bmap_root_held(:big_root) = pg_relation_size('big') / 8192 AS its_length;
SELECT held FROM test_bmap_roots();

-- A relation's first capture logs an image of each page it had not shared
-- before, once in the page's life.  A later capture logs its map work
-- alone: about as much for the big table as for the small one.
SELECT test_bmap_root_capture(:small) AS small_first \gset
CHECKPOINT;
SELECT test_bmap_own_wal() AS mark \gset
SELECT test_bmap_root_capture(:small) AS small_again \gset
SELECT pg_temp.wal_since(:mark) AS small_capture_wal \gset
SELECT test_bmap_own_wal() AS mark \gset
SELECT test_bmap_root_capture(:big) AS big_again \gset
SELECT pg_temp.wal_since(:mark) AS big_capture_wal \gset
SELECT :big_capture_wal < :small_capture_wal + 8192 AS capture_wal_flat;
SELECT test_bmap_root_release(:small_first) AND test_bmap_root_release(:small_again)
	AND test_bmap_root_release(:big_again) AS let_go;

-- More roots than a relation's root page keeps snapshots: all held.
CREATE TEMP TABLE many AS
	SELECT g, test_bmap_root_capture(:small) AS id FROM generate_series(1, 100) g;
SELECT count(*) AS captured, count(DISTINCT id) AS distinct_ids,
	bool_and(test_bmap_root_held(id) IS NOT NULL) AS all_held FROM many;
SELECT held, slots >= held AS slots_cover FROM test_bmap_roots();

-- The relation changes: each page changed is copied, the root keeps it.
-- A relation made of the root holds the root's tree: no map node of its
-- own but its root page.
UPDATE big SET b = 'changed' WHERE a % 100 = 0;
DELETE FROM big WHERE a > 25000;
SELECT test_bmap_rows_digest(:big) <> :'big_rows' AS big_changed;
SELECT test_bmap_share_root(:big_root) AS then \gset
SELECT count(*) AS then_own_nodes FROM test_bmap_tree(:then)
WHERE block NOT IN (SELECT block FROM big_nodes);
SELECT test_bmap_rows_digest(:then) = :'big_rows' AS then_as_it_was;
SELECT test_bmap_write(:then, 10, 1);
SELECT test_bmap_rows_digest(:then) = :'big_rows' AS then_written;
SELECT test_bmap_unlink(:then);

-- The relation dropped: its root outlives it, its pages with it.
DROP TABLE big;
SELECT test_bmap_root_held(:big_root) IS NOT NULL AS held_after_drop;
-- (a sweep frees nothing the root holds; the store may shorten by what
-- the drop let go of)
SELECT freed FROM test_bmap_sweep();
SELECT test_bmap_share_root(:big_root) AS after \gset
SELECT test_bmap_rows_digest(:after) = :'big_rows' AS after_drop_as_it_was;
SELECT test_bmap_unlink(:after);

-- Letting go is the transaction's: rolled back, the root is held still.
BEGIN;
SELECT test_bmap_root_release(:big_root);
SELECT test_bmap_root_held(:big_root) IS NULL AS let_go_in_it;
ROLLBACK;
SELECT test_bmap_root_held(:big_root) IS NOT NULL AS held_after_rollback;
SELECT freed, recounted FROM test_bmap_sweep();
SELECT test_bmap_share_root(:big_root) AS again \gset
SELECT test_bmap_rows_digest(:again) = :'big_rows' AS still_as_it_was;
SELECT test_bmap_unlink(:again);

-- Committed, it is let go of, and once the release queue drains, what only
-- it held is freed.
SELECT test_bmap_root_release(:big_root);
SELECT test_bmap_root_held(:big_root) IS NULL AS let_go;
SELECT test_bmap_root_release(:big_root) AS released_twice;
SELECT freed > 0 AS freed FROM test_bmap_release_drain();
SELECT freed, recounted FROM test_bmap_sweep();

-- A capture whose transaction rolls back holds nothing, and its slot is
-- reclaimed.
BEGIN;
SELECT test_bmap_root_capture(:small) AS rolled_back \gset
SELECT test_bmap_root_held(:rolled_back) IS NOT NULL AS held_in_it;
ROLLBACK;
SELECT test_bmap_root_held(:rolled_back) IS NULL AS not_held;
SELECT test_bmap_roots_reclaim();
SELECT test_bmap_roots_reclaim() AS reclaimed_twice;

-- Slots let go of are reused: as many held as before, no more slots.
SELECT held AS held_before, slots AS slots_before FROM test_bmap_roots() \gset
SELECT count(*) FILTER (WHERE test_bmap_root_release(id)) FROM many WHERE g <= 50;
SELECT count(*) FROM (SELECT test_bmap_root_capture(:small) FROM generate_series(1, 50)) c;
SELECT held = :held_before AS as_many_held, slots = :slots_before AS slots_reused
FROM test_bmap_roots();
SELECT freed, recounted FROM test_bmap_sweep();

-- A capture after one page changed walks the own nodes of the path copied
-- for it, a node of every leaf of the relation among them: their counts are
-- read a count page at a time, not a page read for each child, so what it
-- reads beyond what a capture after no change reads is fewer buffers than
-- the relation has leaves.
CREATE TABLE wide (a int, b text) WITH (fillfactor = 10, autovacuum_enabled = off);
INSERT INTO wide SELECT g, repeat('w', 200) FROM generate_series(1, 30000) g;
SELECT pg_relation_filenode('wide') AS wide \gset
SELECT count(*) FILTER (WHERE level = 0) AS leaves,
	max(level) AS levels
FROM test_bmap_tree(:wide) \gset
SELECT :leaves > 16 AS many_leaves, :levels AS levels;
SELECT test_bmap_root_capture(:wide) AS w1 \gset
-- (what a capture reads with nothing changed since the last: its roots,
-- unnamed, go with the database)
SELECT pg_temp.buffers_read(format('SELECT test_bmap_root_capture(%s)', :wide)) > 0
	AS read_some;
SELECT pg_temp.buffers_read(format('SELECT test_bmap_root_capture(%s)', :wide))
	AS base_buffers \gset
UPDATE wide SET b = 'x' WHERE a = 29000;
SELECT pg_temp.buffers_read(format('SELECT test_bmap_root_capture(%s)', :wide))
	AS capture_buffers \gset
SELECT test_bmap_roots_of(:wide) AS wide_roots;
SELECT :capture_buffers - :base_buffers < :leaves AS counts_read_by_page;
SELECT test_bmap_root_release(:w1) AS let_go;
DROP TABLE wide;
SELECT freed, recounted FROM test_bmap_sweep();

\c :home
DROP DATABASE bmap_captured_roots;
