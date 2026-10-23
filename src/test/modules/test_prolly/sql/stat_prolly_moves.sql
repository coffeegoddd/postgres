-- pg_stat_prolly's counts of what moving between branches does to storage:
-- block maps shared and let go of, rows the row applier writes back and
-- the index entries they make, catalog rows a branch's copies are made of.

-- A database of its own, so nothing else counts in it.
SELECT current_database() AS home \gset
CREATE DATABASE stat_prolly_moves;
\c stat_prolly_moves
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
SET dolt.author = 'Ada Lovelace <ada@example.com>';

-- The work of one statement, counted: the counters it moved.
CREATE FUNCTION pg_temp.counted(names text[]) RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$
	SELECT n, coalesce((SELECT p.value FROM pg_stat_prolly p WHERE p.name = n), -1)
	FROM unnest(names) n
$$;

-- Sharing a relation's blocks copies its map's root page alone, and counts
-- each subtree under it once more (a table this size has one); dropping
-- the copy counts each down again, and frees none (the source holds them).
-- (Fixed-width columns: no TOAST table.)
CREATE SCHEMA sm VERSIONED;
CREATE TABLE sm.b (a int8, b int8, c int8, d int8);
INSERT INTO sm.b SELECT g, g, g, g FROM generate_series(1, 20000) g;
SELECT pg_relation_filenode('sm.b') AS src \gset
SELECT pg_relation_size('sm.b') / current_setting('block_size')::int AS nblocks \gset
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_share(:src) AS copy \gset
SELECT pg_stat_force_next_flush();
SELECT * FROM pg_temp.counted('{bmap_map_pages_copied,bmap_refcount_updates,bmap_blocks_freed}');
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_free_blocks() AS free_before \gset
-- Dropping the copy queues its subtree, and the drain counts it down.
SELECT test_bmap_unlink(:copy);
SELECT pg_stat_force_next_flush();
SELECT * FROM pg_temp.counted('{bmap_map_pages_copied,bmap_refcount_updates,bmap_blocks_freed,bmap_release_queued,bmap_release_drained,bmap_release_blocks_freed,bmap_release_refcount_updates}');
-- Dropping the source, the last holder, does no map work itself; the drain
-- frees every block, counting none down.
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_share(:src) AS copy \gset
SELECT test_bmap_unlink(:copy);
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
DROP TABLE sm.b;
SELECT pg_stat_force_next_flush();
SELECT (SELECT value FROM pg_stat_prolly WHERE name = 'bmap_blocks_freed') = 0 AND
	(SELECT value FROM pg_stat_prolly WHERE name = 'bmap_refcount_updates') = 0
	AS drop_did_no_map_work,
	test_bmap_free_blocks() = :free_before AS drop_freed_none;
SELECT freed >= :nblocks AS drain_freed_all FROM test_bmap_release_drain();
SELECT pg_stat_force_next_flush();
SELECT (SELECT value FROM pg_stat_prolly WHERE name = 'bmap_release_blocks_freed') =
	test_bmap_free_blocks() - :free_before AS freed_counted,
	(SELECT value FROM pg_stat_prolly WHERE name = 'bmap_release_blocks_freed') >= :nblocks AS freed_all,
	(SELECT value FROM pg_stat_prolly WHERE name = 'bmap_release_refcount_updates') = 0
	AS none_counted_down;

-- A table whose updates of v aren't heap-only (an index on v): a row
-- written makes an entry in each of its two indexes.  One whose updates
-- are (nothing indexes w, and its pages have room).  And one whose index
-- on v is partial, holding no row: its updates of v aren't heap-only, and
-- make an entry in the primary key alone.
CREATE TABLE sm.t (id int PRIMARY KEY, v int NOT NULL, w int NOT NULL);
CREATE INDEX ON sm.t (v);
INSERT INTO sm.t SELECT g, 0, 0 FROM generate_series(1, 1000) g;
CREATE TABLE sm.h (id int PRIMARY KEY, w int NOT NULL) WITH (fillfactor = 50);
INSERT INTO sm.h SELECT g, 0 FROM generate_series(1, 1000) g;
CREATE TABLE sm.p (id int PRIMARY KEY, v int NOT NULL);
CREATE INDEX ON sm.p (v) WHERE id < 0;
INSERT INTO sm.p SELECT g, 0 FROM generate_series(1, 1000) g;
SELECT length(dolt_commit('-Am', 'tables')::text) > 0 AS committed;
SELECT dolt_branch('other');

-- A branch's first use copies the session branch's rows of every catalog
-- that holds versioned objects (those with a branch column), and shares
-- every relation's blocks.
CREATE FUNCTION pg_temp.branch_rows(branch oid) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	r record;
	total bigint := 0;
	n bigint;
BEGIN
	FOR r IN
		SELECT a.attrelid::regclass AS catalog, a.attname
		FROM pg_attribute a JOIN pg_class c ON c.oid = a.attrelid
		WHERE c.relnamespace = 'pg_catalog'::regnamespace AND c.relkind = 'r'
			AND a.attnum > 0 AND a.atttypid = 'oid'::regtype
			AND a.attname ~ 'branch$' AND c.relname <> 'pg_branch'
	LOOP
		EXECUTE format('SELECT count(*) FROM %s WHERE %I = $1', r.catalog, r.attname)
			INTO n USING branch;
		total := total + n;
	END LOOP;
	RETURN total;
END $$;
SELECT pg_temp.branch_rows((SELECT oid FROM pg_branch WHERE brname = 'main')) AS main_rows \gset
-- (a root page for each relation with blocks, and a count for each of its
-- subtrees, a relation this size having one: the tables, their indexes and
-- their change logs)
SELECT count(*) AS relations, sum(pg_relation_size(oid)) / current_setting('block_size')::int
	AS blocks
FROM pg_class
WHERE relbranch = (SELECT oid FROM pg_branch WHERE brname = 'main')
	AND relkind IN ('r', 't', 'i', 'm') AND pg_relation_size(oid) > 0 \gset
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT dolt_checkout('other');
SELECT pg_stat_force_next_flush();
SELECT (SELECT value FROM pg_stat_prolly WHERE name = 'catalog_rows_copied') = :main_rows
	AS each_catalog_row_copied,
	(SELECT value FROM pg_stat_prolly WHERE name = 'bmap_map_pages_copied') = :relations
	AS each_map_page_copied,
	(SELECT value FROM pg_stat_prolly WHERE name = 'bmap_refcount_updates') = :relations
	AS each_subtree_counted;
SELECT name, value FROM pg_temp.counted('{apply_rows,apply_index_tuples,bmap_blocks_freed}');

-- A hard reset writes the dirty rows back: three updated, two inserted
-- (deleted again), one deleted (inserted again).  The updates of v and the
-- row inserted again make an entry in both of sm.t's indexes; the deletes
-- none.
UPDATE sm.t SET v = 1 WHERE id IN (1, 2, 3);
INSERT INTO sm.t VALUES (1001, 0, 0), (1002, 0, 0);
DELETE FROM sm.t WHERE id = 500;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT dolt_reset('--hard');
SELECT pg_stat_force_next_flush();
SELECT name, value FROM pg_temp.counted('{apply_rows,apply_index_tuples,apply_loads}');

-- Heap-only updates written back make no index entries.
UPDATE sm.h SET w = 1 WHERE id IN (1, 2, 3, 4);
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT dolt_reset('--hard');
SELECT pg_stat_force_next_flush();
SELECT name, value FROM pg_temp.counted('{apply_rows,apply_index_tuples}');

-- Rows a partial index's predicate leaves out make no entry in it.
UPDATE sm.p SET v = 1 WHERE id IN (1, 2, 3);
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT dolt_reset('--hard');
SELECT pg_stat_force_next_flush();
SELECT name, value FROM pg_temp.counted('{apply_rows,apply_index_tuples}');

-- A table filled from nothing is loaded, its index built after the rows:
-- an entry for each row all the same.  (Its other indexes, made after the
-- rows by the move, aren't the load's.)
CREATE TABLE sm.l (id int PRIMARY KEY, v int NOT NULL);
CREATE INDEX ON sm.l (v);
INSERT INTO sm.l SELECT g, g FROM generate_series(1, 300) g;
SELECT length(dolt_commit('-Am', 'a loaded table')::text) > 0 AS committed;
SELECT dolt_checkout('main');
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT length(dolt_merge('other')::text) > 0 AS merged;
SELECT pg_stat_force_next_flush();
SELECT name, value FROM pg_temp.counted('{apply_rows,apply_index_tuples,apply_loads}');

-- Deleting a branch lets go of its blocks: those another branch holds too
-- are counted down, the rest freed.
SELECT dolt_checkout('other');
UPDATE sm.t SET v = 2 WHERE id <= 100;
SELECT length(dolt_commit('-am', 'changed on other')::text) > 0 AS committed;
SELECT dolt_checkout('main');
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT test_bmap_free_blocks() AS free_before \gset
SELECT dolt_branch('-D', 'other');
SELECT pg_stat_force_next_flush();
-- (the branch's storage queued, its blocks let go of by the drain)
SELECT (SELECT value FROM pg_stat_prolly WHERE name = 'bmap_blocks_freed') = 0 AND
	(SELECT value FROM pg_stat_prolly WHERE name = 'bmap_refcount_updates') = 0
	AS delete_did_no_map_work,
	test_bmap_release_pending() > 0 AS delete_queued;
SELECT entries > 0 AS drained FROM test_bmap_release_drain();
SELECT pg_stat_force_next_flush();
SELECT (SELECT value FROM pg_stat_prolly WHERE name = 'bmap_release_blocks_freed') =
	test_bmap_free_blocks() - :free_before AS freed_counted,
	(SELECT value FROM pg_stat_prolly WHERE name = 'bmap_release_blocks_freed') > 0 AS some_freed,
	(SELECT value FROM pg_stat_prolly WHERE name = 'bmap_release_refcount_updates') > 0 AS some_counted_down,
	(SELECT value FROM pg_stat_prolly WHERE name = 'catalog_rows_copied') = 0 AS nothing_copied;

\c :home
DROP DATABASE stat_prolly_moves;
