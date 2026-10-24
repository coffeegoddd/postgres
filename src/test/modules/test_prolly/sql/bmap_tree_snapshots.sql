-- Snapshots of a tree block map: a reference to a relation's subtrees, kept
-- by its root page.  Keeping one and letting it go are the same map work
-- whatever the relation's size; a new relation made of one has the pages
-- the relation had then; letting it go frees what it alone held.

-- A database of its own, so nothing else counts in it.
SELECT current_database() AS home \gset
CREATE DATABASE bmap_tree_snapshots;
\c bmap_tree_snapshots
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
SET debug_block_mapped_heap = on;

CREATE FUNCTION pg_temp.counted() RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$
	SELECT n, coalesce((SELECT p.value FROM pg_stat_prolly p WHERE p.name = n), -1)
	FROM unnest('{bmap_map_pages_copied,bmap_refcount_updates,bmap_blocks_freed}'::text[]) n
$$;

CREATE TABLE small (a int, b text) WITH (fillfactor = 10);
INSERT INTO small SELECT g, repeat('s', 200) FROM generate_series(1, 3000) g;
CREATE TABLE big (a int, b text) WITH (fillfactor = 10);
INSERT INTO big SELECT g, repeat('b', 200) FROM generate_series(1, 30000) g;
SELECT pg_relation_filenode('small') AS small, pg_relation_filenode('big') AS big \gset
SELECT test_bmap_rows_digest(:big) AS big_rows \gset

-- keeping one: a count for each subtree, no map page copied, whatever the size
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_snapshot_take(:small) AS small_snap \gset
SELECT pg_stat_force_next_flush();
SELECT * FROM pg_temp.counted();
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_snapshot_take(:big) AS big_snap \gset
SELECT pg_stat_force_next_flush();
SELECT * FROM pg_temp.counted();
SELECT count(*), bool_and(nblocks = pg_relation_size('big') / 8192) AS its_length
FROM test_bmap_snapshots(:big);
SELECT * FROM prolly_page_sweep();

-- the relation changes: each page changed is copied, the snapshot keeps it
UPDATE big SET b = 'changed' WHERE a % 100 = 0;
DELETE FROM big WHERE a > 25000;
-- (no VACUUM: it would copy every page the snapshot alone shares, to set it all-visible)
SELECT test_bmap_rows_digest(:big) <> :'big_rows' AS big_changed;
-- a relation made of the snapshot has the pages as they were
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_share_snapshot(:big, :big_snap) AS then \gset
SELECT pg_stat_force_next_flush();
SELECT * FROM pg_temp.counted();
SELECT test_bmap_rows_digest(:then) = :'big_rows' AS then_as_it_was;
SELECT count(*) > 0 AS then_has_nodes FROM test_bmap_tree(:then);
-- and is a relation of its own: changing it changes neither
SELECT test_bmap_write(:then, 10, 1);
SELECT test_bmap_rows_digest(:then) = :'big_rows' AS then_written;
SELECT * FROM prolly_page_sweep();

-- letting the snapshot go: what only it held is freed
SELECT test_bmap_unlink(:then);
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_snapshot_release(:big, :big_snap);
SELECT pg_stat_force_next_flush();
SELECT name, CASE WHEN name = 'bmap_map_pages_copied' THEN value
	ELSE (value > 0)::int END AS value FROM pg_temp.counted();
SELECT test_bmap_snapshot_release(:big, :big_snap) AS released_twice;
SELECT count(*) FROM test_bmap_snapshots(:big);
SELECT * FROM prolly_page_sweep();

-- letting go of a snapshot of an unchanged relation frees nothing
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_snapshot_release(:small, :small_snap);
SELECT pg_stat_force_next_flush();
SELECT * FROM pg_temp.counted();

-- a dropped relation lets go of its snapshots
SELECT test_bmap_snapshot_take(:small) AS s1 \gset
UPDATE small SET b = 'x' WHERE a < 100;
SELECT test_bmap_snapshot_take(:small) AS s2 \gset
SELECT count(*) FROM test_bmap_snapshots(:small);
DROP TABLE small, big;
SELECT * FROM prolly_page_sweep();

-- at most 16
CREATE TABLE many (a int);
INSERT INTO many VALUES (1);
SELECT count(test_bmap_snapshot_take(pg_relation_filenode('many')))
FROM generate_series(1, 16);
DO $$ BEGIN
	PERFORM test_bmap_snapshot_take(pg_relation_filenode('many'));
EXCEPTION WHEN program_limit_exceeded THEN
	RAISE NOTICE 'a 17th is refused';
END $$;
DROP TABLE many;
SELECT * FROM prolly_page_sweep();

\c :home
DROP DATABASE bmap_tree_snapshots;
