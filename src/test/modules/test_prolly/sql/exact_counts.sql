-- A block's reference count is exact however many hold it: one held by
-- every root of a long history (a relation's top node, unchanged across
-- them) counts each, and each root let go of takes one from it as the
-- release queue drains, freeing the block once nothing holds it.  No root
-- let go of makes a page sweep due: freeing keeps up without one.

SELECT current_database() AS home \gset
CREATE DATABASE exact_counts;
\c exact_counts
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
SET debug_block_mapped_heap = on;

CREATE TABLE t (a int, b text);
INSERT INTO t SELECT g, repeat('t', 200) FROM generate_series(1, 2000) g;
SELECT pg_relation_filenode('t') AS t \gset
-- (the sweep a server start makes due, run)
SELECT count(*) >= 0 AS swept FROM prolly_page_sweep();
SELECT test_bmap_sweep_due() AS due;

-- 300 roots of the relation unchanged: its blocks held by each
CREATE TEMP TABLE roots AS
	SELECT g, test_bmap_root_capture(:t) AS id FROM generate_series(1, 300) g;
SELECT min(refs) AS fewest, max(refs) AS most
FROM test_bmap_map(:t);

-- letting go of 290 of them: counted off exactly, no sweep due
SELECT bool_and(test_bmap_root_release(id)) AS released
FROM roots WHERE g <= 290;
SELECT entries > 0 AS drained FROM test_bmap_release_drain();
SELECT min(refs) AS fewest, max(refs) AS most FROM test_bmap_map(:t);
SELECT test_bmap_sweep_due() AS due;

-- the relation dropped, then the last roots let go of: every block freed
-- by the drain, a sweep finding nothing left to free
SELECT count(*) AS blocks FROM test_bmap_map(:t) \gset
SELECT test_bmap_unlink(:t);
SELECT bool_and(test_bmap_root_release(id)) AS released
FROM roots WHERE g > 290;
SELECT freed >= :blocks AS all_freed FROM test_bmap_release_drain();
SELECT test_bmap_sweep_due() AS due;
SELECT freed FROM prolly_page_sweep();

\c :home
DROP DATABASE exact_counts;
