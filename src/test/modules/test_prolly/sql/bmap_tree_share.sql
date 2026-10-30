-- Sharing a block map copies its root page alone: the map work of a share,
-- and of dropping the copy, is the same whatever the relation's size.  The
-- first change under a shared node copies the path to it, a node a level,
-- and the leaf's blocks are then copied as any shared block is.
--
-- The work is measured by the map nodes a step makes (test_bmap_tree()'s
-- blocks) and by the WAL it writes, which would grow with the relation if
-- the step visited each of its blocks.

-- A database of its own, so nothing else changes its page store.
SELECT current_database() AS home \gset
CREATE DATABASE bmap_tree_share;
\c bmap_tree_share
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
SET debug_block_mapped_heap = on;

-- The WAL written since a mark, in bytes.
CREATE FUNCTION pg_temp.wal_since(mark pg_lsn) RETURNS numeric
LANGUAGE sql AS $$ SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), mark) $$;

-- Two tables, of about 1000 and 10000 blocks, each over two subtrees.
-- Autovacuum stays off them: a VACUUM would change their pages, and write
-- WAL, while the work of a step is measured.
CREATE TABLE small (a int, b text) WITH (fillfactor = 10, autovacuum_enabled = off);
INSERT INTO small SELECT g, repeat('s', 200) FROM generate_series(1, 3000) g;
CREATE TABLE big (a int, b text) WITH (fillfactor = 10, autovacuum_enabled = off);
INSERT INTO big SELECT g, repeat('b', 200) FROM generate_series(1, 30000) g;
SELECT pg_relation_filenode('small') AS small, pg_relation_filenode('big') AS big \gset
SELECT pg_relation_size('big') / 8192 AS big_blocks \gset
SELECT pg_relation_size('small') / 8192 > 509 AS small_two_subtrees,
	:big_blocks > 9 * pg_relation_size('small') / 8192 AS big_ten_times;
SELECT test_bmap_rows_digest(:small) AS small_rows,
	test_bmap_rows_digest(:big) AS big_rows \gset
SELECT * FROM test_bmap_sweep();

-- A share makes no map node of the source's: the copy's root page is its
-- own, and the nodes below it are the source's, each held twice.
SELECT test_bmap_share(:small) AS small_copy \gset
SELECT test_bmap_share(:big) AS big_copy \gset
SELECT count(DISTINCT block) AS copy_own_nodes FROM test_bmap_tree(:big_copy)
WHERE block NOT IN (SELECT block FROM test_bmap_tree(:big));
SELECT count(DISTINCT block) AS small_copy_own_nodes FROM test_bmap_tree(:small_copy)
WHERE block NOT IN (SELECT block FROM test_bmap_tree(:small));
SELECT test_bmap_rows_digest(:small_copy) = :'small_rows' AS small_same,
	test_bmap_rows_digest(:big_copy) = :'big_rows' AS big_same;
SELECT level, first, refs FROM test_bmap_tree(:big_copy)
WHERE level = 1 OR first = 0 ORDER BY first, level;

-- A relation's first share logs an image of each page it had not shared
-- before, once in the page's life, which makes the shared block durable.
-- The map work of a share comes on top of that, and is all a later share
-- logs: about as much for the big table as for the small one (within a
-- page image), a count for each subtree rather than for each block.  Dropping such a copy, which
-- holds nothing of its own, logs as little.
CHECKPOINT;
SELECT pg_current_wal_insert_lsn() AS mark \gset
SELECT test_bmap_share(:small) AS small_again \gset
SELECT pg_temp.wal_since(:'mark') AS small_share_wal \gset
SELECT pg_current_wal_insert_lsn() AS mark \gset
SELECT test_bmap_share(:big) AS big_again \gset
SELECT pg_temp.wal_since(:'mark') AS big_share_wal \gset
SELECT :big_share_wal < :small_share_wal + 8192 AS share_wal_flat;
SELECT pg_current_wal_insert_lsn() AS mark \gset
SELECT test_bmap_unlink(:small_again);
SELECT pg_temp.wal_since(:'mark') AS small_drop_wal \gset
SELECT pg_current_wal_insert_lsn() AS mark \gset
SELECT test_bmap_unlink(:big_again);
SELECT pg_temp.wal_since(:'mark') AS big_drop_wal \gset
SELECT :big_drop_wal < :small_drop_wal + 8192 AS drop_wal_flat,
	:big_drop_wal < :big_blocks * 4 AS drop_wal_below_a_count_a_block;

-- The first change under the shared subtree copies its node and the leaf
-- (two map nodes the source didn't have), and the block's own page.
CREATE TEMP TABLE before_write AS SELECT block FROM test_bmap_tree(:big);
SELECT phys AS old_phys FROM test_bmap_map(:big) WHERE blkno = 600 \gset
SELECT test_bmap_write(:big, 600, 1);
SELECT count(DISTINCT block) AS nodes_copied FROM test_bmap_tree(:big)
WHERE block NOT IN (SELECT block FROM before_write);
SELECT phys <> :old_phys AS page_copied FROM test_bmap_map(:big) WHERE blkno = 600;
-- The next change on the same leaf copies no map node.
TRUNCATE before_write;
INSERT INTO before_write SELECT block FROM test_bmap_tree(:big);
SELECT test_bmap_write(:big, 601, 1);
SELECT count(DISTINCT block) AS nodes_copied_again FROM test_bmap_tree(:big)
WHERE block NOT IN (SELECT block FROM before_write);
-- The copy is as it was; the source's path is its own, the copy's shared
-- no more.
SELECT test_bmap_rows_digest(:big_copy) = :'big_rows' AS copy_unchanged;
SELECT (SELECT refs FROM test_bmap_tree(:big) WHERE level = 1) AS source_node_refs,
	(SELECT refs FROM test_bmap_tree(:big_copy) WHERE level = 1) AS copy_node_refs;
SELECT * FROM test_bmap_sweep();

-- Dropping a copy lets go of a count for each subtree, by the drain of
-- the subtrees the drop queued: it frees nothing the source holds.
SELECT test_bmap_free_blocks() AS free_before \gset
SELECT test_bmap_unlink(:small_copy);
SELECT test_bmap_free_blocks() - :free_before AS small_copy_freed;
SELECT test_bmap_rows_digest(:small) = :'small_rows' AS small_unchanged;
SELECT bool_and(refs = 0) AS small_alone FROM test_bmap_map(:small);
-- The big copy: the subtree the source copied away is the copy's alone,
-- so it is freed with the copy, down to the blocks only it held; the other
-- subtrees only lose a count.
SELECT test_bmap_free_blocks() AS free_before \gset
SELECT test_bmap_unlink(:big_copy);
SELECT test_bmap_free_blocks() - :free_before > 0 AS big_copy_freed_its_own;
SELECT bool_and(refs = 0) AS big_alone FROM test_bmap_map(:big);
SELECT freed, recounted FROM test_bmap_sweep();

-- Copies of copies, the first dropped first.
SELECT test_bmap_share(:small) AS c1 \gset
SELECT test_bmap_share(:c1) AS c2 \gset
SELECT test_bmap_write(:c1, 3, 1);
SELECT test_bmap_unlink(:c1);
SELECT test_bmap_rows_digest(:c2) = :'small_rows' AS c2_same;
SELECT test_bmap_unlink(:c2);
SELECT * FROM test_bmap_sweep();

\c :home
DROP DATABASE bmap_tree_share;
