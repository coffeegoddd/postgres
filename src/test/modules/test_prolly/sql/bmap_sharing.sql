-- Block-mapped storage shares pages between copies: a copy's map holds its
-- source's subtrees, each counted once more (test_bmap_map()'s refs adds up
-- the counts on a block's path: the maps besides this one that reach it),
-- and a block is freed only when the last map holding it goes.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA cow;
SET debug_block_mapped_heap = on;
-- (fixed-width columns and no index: no TOAST table or index, whose blocks
-- would count too)
CREATE TABLE cow.t (a int8, b int8, c int8, d int8);
INSERT INTO cow.t SELECT g, g, g, g FROM generate_series(1, 20000) g;
SELECT pg_relation_filenode('cow.t') AS src \gset
SELECT pg_relation_size('cow.t') / 8192 AS nblocks \gset
SELECT count(DISTINCT block) AS src_nodes FROM test_bmap_tree(:src) \gset

-- A copy reads the same pages, from the same blocks, each now counted once.
SELECT test_bmap_share(:src) AS copy \gset
SELECT test_bmap_digest(:src) = test_bmap_digest(:copy) AS same_pages;
SELECT count(*) = :nblocks AS all_shared
  FROM test_bmap_map(:src) s JOIN test_bmap_map(:copy) c USING (blkno)
  WHERE s.phys = c.phys AND s.refs = 1 AND c.refs = 1;
-- Both maps' epochs pass every entry's birth: no block is theirs alone.
SELECT test_bmap_epoch(:src) AS src_epoch, test_bmap_epoch(:copy) AS copy_epoch;
SELECT bool_and(epoch < 1) AS entries_older FROM test_bmap_map(:copy);

-- Dropping the source frees none of its blocks, which the copy holds.  The
-- drop queues them, to be let go of by a drain: itself it frees nothing,
-- and leaves the copy's counts as they were.
-- (what earlier tests dropped, let go of first: after a checkpoint, so that
-- any blocks copied from without an image are freed too)
CHECKPOINT;
SELECT entries >= 0 AS drained_before FROM test_bmap_release_drain();
SELECT test_bmap_free_blocks() AS free_before \gset
SELECT test_bmap_digest(:copy) AS copy_digest \gset
SELECT test_bmap_release_pending() AS pending_before \gset
DROP TABLE cow.t;
SELECT test_bmap_free_blocks() = :free_before AS drop_freed_none,
       test_bmap_release_pending() > :pending_before AS drop_queued;
SELECT bool_and(refs = 1) AS copy_counts_kept FROM test_bmap_map(:copy);
SELECT freed AS drain_freed FROM test_bmap_release_drain();
SELECT test_bmap_release_pending() AS pending_drained;
SELECT test_bmap_free_blocks() = :free_before AS nothing_freed;
SELECT test_bmap_digest(:copy) = :'copy_digest' AS copy_intact;
SELECT bool_and(refs = 0) AS copy_holds_alone FROM test_bmap_map(:copy);

-- Dropping the copy, the last holder, frees them.
SELECT test_bmap_unlink(:copy);
SELECT test_bmap_free_blocks() - :free_before =
	:nblocks + :src_nodes AS all_freed;

-- A copy of a copy: three maps hold the blocks.
CREATE TABLE cow.u (v text);
INSERT INTO cow.u SELECT repeat('y', 200) FROM generate_series(1, 1000);
SELECT pg_relation_filenode('cow.u') AS u \gset
SELECT pg_relation_size('cow.u') / 8192 AS unblocks \gset
SELECT test_bmap_digest(:u) AS u_digest \gset
SELECT test_bmap_share(:u) AS c1 \gset
SELECT test_bmap_share(:c1) AS c2 \gset
SELECT bool_and(refs = 2) AS three_holders FROM test_bmap_map(:c2);

-- Truncating a copy leaves its entries past the end; extending it again
-- lets go of those another map holds and allocates new blocks for them.
SELECT test_bmap_truncate(:c2, 2);
SELECT test_bmap_extend(:c2, :unblocks - 2);
SELECT count(*) FILTER (WHERE c.phys = s.phys) AS c2_still_shared,
       count(*) FILTER (WHERE c.phys <> s.phys) AS c2_own
  FROM test_bmap_map(:c1) s JOIN test_bmap_map(:c2) c USING (blkno);
SELECT count(*) FILTER (WHERE refs = 2) AS held_by_three,
       count(*) FILTER (WHERE refs = 1) AS held_by_two
  FROM test_bmap_map(:c1);
SELECT test_bmap_digest(:u) = :'u_digest' AS u_intact;
SELECT test_bmap_digest(:c1) = :'u_digest' AS c1_intact;
-- The extension zeroed the copy's new pages, not the shared ones.
SELECT count(*) AS c2_new_blocks_zeroed FROM generate_series(2, :unblocks - 1) b
  WHERE (SELECT phys FROM test_bmap_map(:c2) WHERE blkno = b) NOT IN
        (SELECT phys FROM test_bmap_map(:u));

-- An entry past the end that no other map holds is claimed, not copied:
-- the same block, born again in the map's epoch.
SELECT test_bmap_unlink(:c2);
SELECT test_bmap_unlink(:c1);
SELECT test_bmap_share(:u) AS c3 \gset
SELECT test_bmap_unlink(:c3);
SELECT bool_and(refs = 0) AS u_alone_again FROM test_bmap_map(:u);
SELECT phys AS u_phys_last FROM test_bmap_map(:u) WHERE blkno = :unblocks - 1 \gset
SELECT test_bmap_truncate(:u, 1);
SELECT test_bmap_extend(:u, :unblocks - 1);
SELECT phys = :u_phys_last AS same_block, epoch = test_bmap_epoch(:u) AS born_again
  FROM test_bmap_map(:u) WHERE blkno = :unblocks - 1;

DROP SCHEMA cow CASCADE;
RESET debug_block_mapped_heap;
