-- What the first change of a page costs block-mapped storage, beyond what
-- it costs a relation of md.c's: buffers read and records logged per page.
-- A page shared with a copy is copied (one record, the map's pages and the
-- allocator's and reference counts' read once); a page whose other holder
-- went is claimed (one record).  Its entry is looked up once, and the path
-- to it read again only when it isn't stamped in the map's epoch.  Counts,
-- not times: they hold on any machine.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
CREATE EXTENSION IF NOT EXISTS pg_walinspect;
RESET client_min_messages;

CREATE SCHEMA fw;

-- Buffers read (hit or not) and WAL records of an UPDATE of one row on each
-- of the table's first 'pages' pages, as EXPLAIN counts them.
CREATE FUNCTION fw.first_writes(rel regclass, pages int,
	OUT buffers int8, OUT records int8)
LANGUAGE plpgsql AS $$
DECLARE
	line text;
	m text[];
BEGIN
	buffers := 0;
	records := 0;
	FOR line IN EXECUTE format(
		'EXPLAIN (ANALYZE, BUFFERS, WAL, COSTS OFF, TIMING OFF, SUMMARY OFF) '
		'UPDATE %s SET a = a WHERE ctid = ANY (ARRAY(SELECT format(''(%%s,1)'', g)::tid '
		'FROM generate_series(0, %s - 1) g))', rel, pages)
	LOOP
		-- (the plan's top node: the first lines of each kind)
		IF buffers = 0 AND line ~ 'Buffers: shared' THEN
			m := regexp_match(line, 'hit=(\d+)');
			buffers := buffers + coalesce(m[1]::int8, 0);
			m := regexp_match(line, 'read=(\d+)');
			buffers := buffers + coalesce(m[1]::int8, 0);
		END IF;
		IF records = 0 AND line ~ 'WAL: records=' THEN
			records := (regexp_match(line, 'records=(\d+)'))[1]::int8;
		END IF;
	END LOOP;
END
$$;

-- Three tables of the same 200 pages, each page holding a few rows with
-- room for another version: md.c's, one block-mapped and shared with a
-- copy, and one block-mapped whose copy was made and dropped.
CREATE TABLE fw.plain (a int, b text) WITH (fillfactor = 50);
SET debug_block_mapped_heap = on;
CREATE TABLE fw.shared (a int, b text) WITH (fillfactor = 50);
CREATE TABLE fw.dropped (a int, b text) WITH (fillfactor = 50);
RESET debug_block_mapped_heap;
INSERT INTO fw.plain SELECT g, repeat('x', 1000) FROM generate_series(1, 600) g;
INSERT INTO fw.shared SELECT * FROM fw.plain;
INSERT INTO fw.dropped SELECT * FROM fw.plain;
VACUUM (FREEZE) fw.plain, fw.shared, fw.dropped;
SELECT pg_relation_filenode('fw.shared') AS shared,
       pg_relation_filenode('fw.dropped') AS dropped \gset
SELECT pg_relation_size('fw.plain') / 8192 AS pages \gset
SELECT :pages = pg_relation_size('fw.shared') / 8192 AND
       :pages = pg_relation_size('fw.dropped') / 8192 AS same_pages;

SELECT test_bmap_share(:shared) AS shared_copy \gset
SELECT test_bmap_share(:dropped) AS dropped_copy \gset
SELECT test_bmap_unlink(:dropped_copy);
-- (past any copies made without images, so the dropped copy's pages are
-- claimed, not copied)
CHECKPOINT;

SELECT * FROM fw.first_writes('fw.plain', :pages) \gset plain_
SELECT test_bmap_refs_file_asks() AS asks_before \gset
SELECT * FROM fw.first_writes('fw.shared', :pages) \gset shared_
SELECT test_bmap_refs_file_asks() AS asks_shared \gset
SELECT * FROM fw.first_writes('fw.dropped', :pages) \gset dropped_
SELECT test_bmap_refs_file_asks() AS asks_dropped \gset

-- Every page was copied, or claimed: its entry born in the map's epoch.
SELECT count(*) FILTER (WHERE epoch < test_bmap_epoch(:shared)) AS shared_not_copied
  FROM test_bmap_map(:shared) WHERE blkno < :pages;
SELECT count(*) FILTER (WHERE epoch < test_bmap_epoch(:dropped)) AS dropped_not_claimed
  FROM test_bmap_map(:dropped) WHERE blkno < :pages;

-- A copy reads at most eight buffers more a page than md.c's change of it:
-- the header, the path to the leaf (the root page and the leaf, for these
-- pages), the old block's count, the allocator's metapage and bitmap page,
-- and the leaf again to change it, seven, and what copying the paths and
-- the visibility map's page cost.  (Looking the entry up before the header
-- was held, then again under it, and reading the path a third time to see
-- it was the map's own, made it eleven.)
SELECT (:shared_buffers - :plain_buffers) <= 8 * :pages AS shared_buffers_bounded;
-- A claim reads at most six: the header, the path, the block's count, and
-- the leaf to change it, five, and the paths stamped.  (It was nine.)
SELECT (:dropped_buffers - :plain_buffers) <= 6 * :pages AS dropped_buffers_bounded;

-- The reference counts' file is asked its length a few times at most, not
-- a page: whether a block's count page exists is answered from the length
-- known (it was asked, with the file closed and opened again, at each
-- page's first change).
SELECT :asks_shared - :asks_before <= 4 AS copies_ask_the_counts_file_little;
SELECT :asks_dropped - :asks_shared <= 4 AS claims_ask_the_counts_file_little;

-- One record a page more than md.c's: the copy's, or the claim's, plus a
-- few records copying the table's paths and its visibility map's page,
-- or, its copy dropped, moving them (a node another map may have shared
-- moves to a new block: its record and its old block's freeing).
SELECT :shared_records - :plain_records - :pages <= 10 AS shared_one_record_per_page;
SELECT :dropped_records - :plain_records - :pages <= 14 AS dropped_one_record_per_page;

SELECT test_bmap_unlink(:shared_copy);
DROP SCHEMA fw CASCADE;
