-- Sharing a block map copies its root page
-- alone: the map work of a share, and of dropping the copy, is the same
-- whatever the relation's size.  The first change under a shared node
-- copies the path to it, a node a level, and the leaf's blocks are then
-- copied as any shared block is.

-- A database of its own, so nothing else counts in it.
SELECT current_database() AS home \gset
CREATE DATABASE bmap_tree_share;
\c bmap_tree_share
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
SET debug_block_mapped_heap = on;

-- The work of one statement, counted: the counters it moved.
CREATE FUNCTION pg_temp.counted() RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$
	SELECT n, coalesce((SELECT p.value FROM pg_stat_prolly p WHERE p.name = n), -1)
	FROM unnest('{bmap_map_pages_copied,bmap_refcount_updates,bmap_blocks_freed}'::text[]) n
$$;

-- two tables, of about 1000 and 10000 blocks, each over two subtrees
CREATE TABLE small (a int, b text) WITH (fillfactor = 10);
INSERT INTO small SELECT g, repeat('s', 200) FROM generate_series(1, 3000) g;
CREATE TABLE big (a int, b text) WITH (fillfactor = 10);
INSERT INTO big SELECT g, repeat('b', 200) FROM generate_series(1, 30000) g;
SELECT pg_relation_filenode('small') AS small, pg_relation_filenode('big') AS big \gset
SELECT pg_relation_size('small') / 8192 > 509 AS small_two_subtrees,
	pg_relation_size('big') / 8192 > 9 * pg_relation_size('small') / 8192 AS big_ten_times;
SELECT test_bmap_rows_digest(:small) AS small_rows,
	test_bmap_rows_digest(:big) AS big_rows \gset
SELECT * FROM prolly_page_sweep();

-- a share: one map page, a count for each subtree
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_share(:small) AS small_copy \gset
SELECT pg_stat_force_next_flush();
SELECT * FROM pg_temp.counted();
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_share(:big) AS big_copy \gset
SELECT pg_stat_force_next_flush();
SELECT * FROM pg_temp.counted();
SELECT test_bmap_rows_digest(:small_copy) = :'small_rows' AS small_same,
	test_bmap_rows_digest(:big_copy) = :'big_rows' AS big_same;
-- (the copy's subtrees are the source's, each held twice)
SELECT level, first, refs FROM test_bmap_tree(:big_copy)
WHERE level = 1 OR first = 0 ORDER BY first, level;

-- the first change under the shared subtree copies its node and the leaf:
-- two map pages, a count for each child of each, and the block's own copy
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_write(:big, 600, 1);
SELECT pg_stat_force_next_flush();
SELECT name, CASE WHEN name = 'bmap_refcount_updates'
	THEN value - (SELECT count(*) FROM test_bmap_tree(:big) WHERE level = 0 AND first >= 509)
		- (SELECT count(*) FROM test_bmap_map(:big) WHERE blkno BETWEEN 509 AND 1017)
	ELSE value END AS value
FROM pg_temp.counted();
-- the next change on the same leaf copies no map page
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_write(:big, 601, 1);
SELECT pg_stat_force_next_flush();
SELECT * FROM pg_temp.counted();
-- the copy is as it was; the source's path is its own, the copy's shared no more
SELECT test_bmap_rows_digest(:big_copy) = :'big_rows' AS copy_unchanged;
SELECT (SELECT refs FROM test_bmap_tree(:big) WHERE level = 1) AS source_node_refs,
	(SELECT refs FROM test_bmap_tree(:big_copy) WHERE level = 1) AS copy_node_refs;
SELECT * FROM prolly_page_sweep();

-- dropping a copy: a count less for each subtree, whatever the size
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_unlink(:small_copy);
SELECT pg_stat_force_next_flush();
SELECT * FROM pg_temp.counted();
SELECT test_bmap_rows_digest(:small) = :'small_rows' AS small_unchanged;
-- the big copy: the subtree the source copied away is the copy's alone, so
-- it is freed with the copy, down to the blocks only it held
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_unlink(:big_copy);
SELECT pg_stat_force_next_flush();
SELECT name, value > 0 AS moved FROM pg_temp.counted();
SELECT * FROM prolly_page_sweep();

-- copies of copies, the first dropped first
SELECT test_bmap_share(:small) AS c1 \gset
SELECT test_bmap_share(:c1) AS c2 \gset
SELECT test_bmap_write(:c1, 3, 1);
SELECT test_bmap_unlink(:c1);
SELECT test_bmap_rows_digest(:c2) = :'small_rows' AS c2_same;
SELECT test_bmap_unlink(:c2);
SELECT * FROM prolly_page_sweep();

\c :home
DROP DATABASE bmap_tree_share;
