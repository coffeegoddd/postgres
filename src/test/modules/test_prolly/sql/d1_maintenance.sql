--
-- ANALYZE, TABLESAMPLE, bitmap scans and CLUSTER on engine D1 tables:
-- samples come from positions in the data tree, bitmaps from TIDs.
--
SET default_table_access_method = prolly_d1;
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

-- ANALYZE of a table larger than its sample: the statistics of a sample,
-- and the row count, exact (it is the tree's item count).
CREATE TABLE big (id int PRIMARY KEY, k int);
INSERT INTO big SELECT g, g % 10 FROM generate_series(1, 100000) g;
ANALYZE big;
SELECT reltuples FROM pg_class WHERE relname = 'big';
SELECT n_distinct, (SELECT array_agg(v ORDER BY v)
					FROM unnest(most_common_vals::text::int[]) v) AS common
FROM pg_stats WHERE tablename = 'big' AND attname = 'k';
SELECT n_distinct FROM pg_stats WHERE tablename = 'big' AND attname = 'id';

-- The sample spreads over the whole table, also when it is close to the
-- table's size (statistics target 10: 3000 rows).
CREATE TABLE near (id int PRIMARY KEY);
ALTER TABLE near ALTER id SET STATISTICS 10;
INSERT INTO near SELECT generate_series(1, 5000);
ANALYZE near;
SELECT histogram_bounds[1]::int < 50 AS low, histogram_bounds[11]::int > 4950 AS high
FROM (SELECT histogram_bounds::text::int[] AS histogram_bounds FROM pg_stats
	  WHERE tablename = 'near') s;

-- Identical rows share an item, and the sample keeps a fair share of
-- every item's rows: it reaches the highest values too.
CREATE TABLE triples (a int, b text);
ALTER TABLE triples ALTER a SET STATISTICS 10, ALTER b SET STATISTICS 10;
INSERT INTO triples SELECT g % 40000, 'x' FROM generate_series(1, 120000) g;
ANALYZE triples;
SELECT reltuples FROM pg_class WHERE relname = 'triples';
-- The planner counts rows, not items: the rows per page ANALYZE found,
-- scaled to the table's size now.
CREATE FUNCTION planned_rows(query text) RETURNS float8
LANGUAGE plpgsql AS $$
DECLARE
	plan json;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || query INTO plan;
	RETURN (plan -> 0 -> 'Plan' ->> 'Plan Rows')::float8;
END $$;
SELECT planned_rows('SELECT * FROM triples') BETWEEN 110000 AND 130000 AS rows_planned;
INSERT INTO triples SELECT g % 40000 + 40000, 'x' FROM generate_series(1, 120000) g;
SELECT planned_rows('SELECT * FROM triples') BETWEEN 220000 AND 260000 AS rows_planned;
DELETE FROM triples WHERE a >= 40000;
-- (Taking every row of the first items read instead would see each value
-- exactly three times, and estimate as many distinct values as it saw.)
SELECT (histogram_bounds::text::int[])[11] > 39000 AS reaches_top,
	   (histogram_bounds::text::int[])[1] < 1000 AS reaches_bottom,
	   n_distinct < 0 OR n_distinct > 3000 AS scaled_distinct
FROM pg_stats WHERE tablename = 'triples' AND attname = 'a';

-- After the transaction deletes most rows, the sample still holds each
-- row once (positions come from the tree without its writes).
BEGIN;
DELETE FROM big WHERE id % 10 <> 0;
ANALYZE big;
SELECT n_distinct FROM pg_stats WHERE tablename = 'big' AND attname = 'id';
ROLLBACK;

-- TABLESAMPLE: every row exactly once at 100%, with the transaction's
-- deletes, updates and inserts (also many inserts past the table's end).
SELECT count(*), count(DISTINCT id), sum(id) FROM big TABLESAMPLE SYSTEM (100);
BEGIN;
DELETE FROM big WHERE id % 3 = 0;
UPDATE big SET k = -1 WHERE id % 7 = 0;
INSERT INTO big SELECT g, 0 FROM generate_series(100001, 110000) g;
SELECT count(*), count(DISTINCT id), sum(id) FROM big TABLESAMPLE SYSTEM (100);
SELECT count(*), count(DISTINCT id), sum(id) FROM big TABLESAMPLE BERNOULLI (100);
SELECT count(*) FROM big TABLESAMPLE SYSTEM (100) WHERE k = -1;
SELECT count(*) FROM big;
ROLLBACK;
SELECT count(*) BETWEEN 7000 AND 13000 AS about_a_tenth
FROM big TABLESAMPLE SYSTEM (10) REPEATABLE (5);
SELECT count(*) BETWEEN 8000 AND 12000 AS about_a_tenth
FROM big TABLESAMPLE BERNOULLI (10) REPEATABLE (5);
-- A sample scan started again before it ends starts from the beginning.
SELECT x, (SELECT id FROM big TABLESAMPLE SYSTEM (100) LIMIT 1 OFFSET x),
	   (SELECT id FROM big TABLESAMPLE BERNOULLI (100) LIMIT 1 OFFSET x)
FROM generate_series(0, 2) x;
-- The same seed picks the same rows.
SELECT (SELECT sum(id) FROM big TABLESAMPLE SYSTEM (10) REPEATABLE (5)) =
	   (SELECT sum(id) FROM big TABLESAMPLE SYSTEM (10) REPEATABLE (5)) AS repeatable;

-- Bitmap scans of prolly indexes, combined.
CREATE TABLE bm (id int PRIMARY KEY, g int, s text);
INSERT INTO bm SELECT i, i % 50, 'x' || (i % 7) FROM generate_series(1, 50000) i;
CREATE INDEX bm_g ON bm USING prolly (g);
CREATE INDEX bm_s ON bm USING prolly (s);
ANALYZE bm;
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM bm WHERE g = 3 OR s = 'x2';
SELECT count(*), sum(id) FROM bm WHERE g = 3 OR s = 'x2';
SELECT count(*), sum(id) FROM bm WHERE g = 3 AND s = 'x2';
BEGIN;
DELETE FROM bm WHERE id % 2 = 0;
UPDATE bm SET s = 'x2' WHERE id % 3 = 0;
SELECT count(*), sum(id) FROM bm WHERE g = 3 OR s = 'x2';
ROLLBACK;
-- Lossy pages: every row of a page is looked up, and rechecked.
CREATE TABLE wide (id int PRIMARY KEY, g int);
INSERT INTO wide SELECT i, i % 3 FROM generate_series(1, 400000) i;
CREATE INDEX wide_g ON wide USING prolly (g);
ANALYZE wide;
SET work_mem = '64kB';
CREATE FUNCTION lossy_blocks(query text) RETURNS bool
LANGUAGE plpgsql AS $$
DECLARE
	plan json;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, TIMING OFF) ' || query INTO plan;
	RETURN (plan -> 0 -> 'Plan' -> 'Plans' -> 0 ->> 'Lossy Heap Blocks')::int > 0;
END $$;
SELECT lossy_blocks('SELECT count(*) FROM wide WHERE g = 1');
SELECT count(*), sum(id) FROM wide WHERE g = 1;
RESET work_mem;
-- A bitmap scan started again before it ends starts from its new bitmap
-- (also for the same key again, whose rows would pass a recheck).
SELECT x, (SELECT id FROM bm WHERE g = x LIMIT 1 OFFSET 1)
FROM (VALUES (1), (1), (2)) v(x);
RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;
SELECT count(*), sum(id) FROM bm WHERE g = 3 OR s = 'x2';
SELECT count(*), sum(id) FROM bm WHERE g < 25 OR s = 'x2';

-- CLUSTER and VACUUM FULL give the new storage the same trees (rows are
-- in key order already), and the same TIDs; the indexes still agree.
CREATE TEMP TABLE digests USING heap AS
	SELECT c::text AS rel, prolly_tree_digest(c) AS digest
	FROM unnest(ARRAY['bm', 'bm_g', 'bm_s']::regclass[]) c
	UNION ALL
	SELECT 'tids', md5(string_agg(ctid::text, ',' ORDER BY id)) FROM bm;
CREATE FUNCTION same_as_before() RETURNS TABLE (rel text, same bool)
LANGUAGE sql AS $$
	SELECT rel, CASE rel WHEN 'tids' THEN
		(SELECT md5(string_agg(ctid::text, ',' ORDER BY id)) FROM bm)
		ELSE prolly_tree_digest(rel::regclass) END = digest
	FROM digests ORDER BY rel
$$;
CLUSTER bm USING bm_g;
SELECT * FROM same_as_before();
VACUUM FULL bm;
SELECT * FROM same_as_before();
SET enable_seqscan = off;
SELECT count(*), sum(id) FROM bm WHERE g = 3;
RESET enable_seqscan;
BEGIN;
DELETE FROM bm WHERE id <= 10;
INSERT INTO bm VALUES (90000, 3, 'x0');
CLUSTER bm USING bm_g;
SELECT count(*), sum(id) FROM bm;
SET LOCAL enable_seqscan = off;
SELECT count(*), sum(id) FROM bm WHERE g = 3;
-- New storage, written in this transaction, is sampled from where the
-- transaction keeps it.
ANALYZE bm;
SELECT reltuples FROM pg_class WHERE relname = 'bm';
SELECT count(*) BETWEEN 2000 AND 8000 AS about_a_tenth
FROM bm TABLESAMPLE SYSTEM (10) REPEATABLE (1);
ROLLBACK;
SELECT * FROM same_as_before();
VACUUM FULL triples;
SELECT count(*), count(DISTINCT a) FROM triples;
SELECT reltuples FROM pg_class WHERE relname = 'triples';

-- A table whose rows are all the transaction's own is sampled too.
BEGIN;
CREATE TABLE fresh (id int PRIMARY KEY);
INSERT INTO fresh SELECT generate_series(1, 5000);
SELECT count(*), sum(id) FROM fresh TABLESAMPLE SYSTEM (100);
SELECT count(*), sum(id) FROM fresh TABLESAMPLE BERNOULLI (100);
ROLLBACK;

DROP FUNCTION same_as_before();
DROP FUNCTION lossy_blocks(text);
DROP FUNCTION planned_rows(text);
DROP TABLE big, near, triples, bm, wide, digests;
