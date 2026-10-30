-- Diffs of two block maps of a page store, relations' or held roots': the
-- logical blocks whose pages differ, found by comparing the maps' pointers,
-- in time proportional to what differs, not to the maps' sizes.
CREATE EXTENSION IF NOT EXISTS test_prolly;
SET debug_block_mapped_heap = on;

-- The logical blocks two maps differ in, worked out the slow way from every
-- entry of each, to check a diff against.
CREATE FUNCTION diff_by_entries(a oid, b oid) RETURNS int8[]
LANGUAGE sql AS $$
	SELECT coalesce(array_agg(blkno ORDER BY blkno), '{}')
	FROM test_bmap_map(a) ma FULL JOIN test_bmap_map(b) mb USING (blkno)
	WHERE ma.phys IS DISTINCT FROM mb.phys
$$;
CREATE FUNCTION diff_of(a oid, b oid) RETURNS int8[]
LANGUAGE sql AS $$
	SELECT coalesce(array_agg(d), '{}') FROM test_bmap_diff(a, b) d
$$;

CREATE TABLE diff_t (id int, pad text)
	WITH (autovacuum_enabled = off, fillfactor = 50);
INSERT INTO diff_t SELECT g, repeat('d', 200) FROM generate_series(1, 2000) g;
SELECT relfilenode AS t FROM pg_class WHERE relname = 'diff_t' \gset
SELECT pg_relation_size('diff_t') / current_setting('block_size')::int AS t_blocks \gset
SELECT :t_blocks > 100 AS enough_blocks;

-- A map against itself, and two shares of it before any changes: nothing
-- differs.
SELECT test_bmap_share(:t) AS s1, test_bmap_share(:t) AS s2 \gset
SELECT diff_of(:t, :t) AS itself, diff_of(:t, :s1) AS share,
	diff_of(:s1, :s2) AS shares;

-- A few pages changed on each side: exactly those, whichever side is first.
SELECT test_bmap_write(:s1, 3, 1), test_bmap_write(:s1, 40, 1);
SELECT test_bmap_write(:s2, 40, 2), test_bmap_write(:s2, 100, 2);
SELECT diff_of(:s1, :s2) AS changed, diff_of(:s2, :s1) AS reversed,
	diff_of(:t, :s1) AS s1_from_t, diff_of(:t, :s2) AS s2_from_t;
SELECT diff_of(:s1, :s2) = diff_by_entries(:s1, :s2) AS agrees;

-- Lengths: blocks past one map's length and under the other's differ, even
-- where both still share the subtree mapping them; past both, nothing does.
SELECT test_bmap_extend(:s2, 5);
SELECT diff_of(:s1, :s2) = ARRAY[3, 40, 100] ||
	array(SELECT generate_series(:t_blocks, :t_blocks + 4)::int8)
	AS extended;
SELECT test_bmap_truncate(:s1, 50, true);
SELECT diff_of(:s1, :s2) = ARRAY[3, 40] ||
	array(SELECT generate_series(50, :t_blocks + 4)::int8)
	AS truncated;
SELECT diff_of(:s1, :s2) = diff_by_entries(:s1, :s2) AS agrees;
SELECT test_bmap_truncate(:s2, 50, true);
SELECT diff_of(:s1, :s2) AS both_truncated;

-- Against a held root: the pages changed since it was captured, and its
-- length's difference; two roots against each other, as their relations
-- were.
BEGIN;
SELECT test_bmap_root_capture(:s1) AS r1, test_bmap_root_capture(:s2) AS r2 \gset
COMMIT;
SELECT array_agg(d) AS since_r1 FROM test_bmap_diff_root(:s1, :r1) d;
SELECT test_bmap_write(:s1, 7, 3), test_bmap_write(:s1, 41, 3);
SELECT test_bmap_extend(:s1, 2);
SELECT array_agg(d) AS since_r1 FROM test_bmap_diff_root(:s1, :r1) d;
SELECT array_agg(d) AS r1_r2 FROM test_bmap_diff_roots(:r1, :r2) d;
SELECT array_agg(d) AS r1_r1 FROM test_bmap_diff_roots(:r1, :r1) d;
SELECT array_agg(d) = ARRAY[40] ||
	array(SELECT generate_series(50, :t_blocks - 1)::int8) AS t_r2
FROM test_bmap_diff_root(:t, :r2) d;

-- Storage that isn't block-mapped can't be compared.
SET debug_block_mapped_heap = off;
CREATE TABLE plain_t (id int);
SET debug_block_mapped_heap = on;
SELECT relfilenode AS p FROM pg_class WHERE relname = 'plain_t' \gset
CREATE FUNCTION diff_refused(a oid, b oid) RETURNS bool
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM test_bmap_diff(a, b);
	RETURN false;
EXCEPTION WHEN feature_not_supported THEN
	RETURN true;
END
$$;
SELECT diff_refused(:t, :p) AS refused;

-- What a diff costs: the map pages above the pages that differ, not the
-- maps' sizes.  A table of thousands of pages (each a row long, so that
-- most of its pages lie under a subtree a level deeper than the small
-- table's) against its share with one page changed reads the same map
-- pages as the small table does, but for one more level on each side; a
-- walk of the whole maps would read every leaf of each.
CREATE TABLE big_t (id int, pad text)
	WITH (autovacuum_enabled = off, fillfactor = 10);
INSERT INTO big_t SELECT g, repeat('b', 700) FROM generate_series(1, 5200) g;
SELECT relfilenode AS big FROM pg_class WHERE relname = 'big_t' \gset
SELECT pg_relation_size('big_t') / current_setting('block_size')::int >= 5000
	AS big_has_ten_leaves;
SELECT test_bmap_share(:big) AS big_share, test_bmap_share(:t) AS small_share \gset
SELECT test_bmap_diff_reads(:big, :big_share) AS unchanged_reads \gset
SELECT :unchanged_reads <= 4 AS unchanged_reads_headers_and_roots;
SELECT test_bmap_write(:big_share, 3000, 1);
SELECT test_bmap_write(:small_share, 40, 1);
SELECT diff_of(:big, :big_share) AS big_diff, diff_of(:t, :small_share) AS small_diff;
SELECT test_bmap_diff_reads(:big, :big_share) AS big_reads \gset
SELECT test_bmap_diff_reads(:t, :small_share) AS small_reads \gset
SELECT :big_reads <= :small_reads + 2 AS reads_flat_in_size;

SELECT test_bmap_unlink(:big_share);
SELECT test_bmap_unlink(:small_share);
BEGIN;
SELECT test_bmap_root_release(r) FROM unnest(ARRAY[:r1, :r2]::int8[]) r;
COMMIT;
SELECT test_bmap_unlink(:s1);
SELECT test_bmap_unlink(:s2);
DROP TABLE diff_t, big_t, plain_t;
DROP FUNCTION diff_by_entries(oid, oid), diff_of(oid, oid), diff_refused(oid, oid);
SELECT entries >= 0 AS drained FROM test_bmap_release_drain();
RESET debug_block_mapped_heap;
