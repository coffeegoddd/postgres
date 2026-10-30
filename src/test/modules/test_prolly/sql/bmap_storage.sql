-- Block-mapped storage chosen per relation: with debug_block_mapped_heap,
-- new user heap tables, their indexes and their TOAST tables are
-- block-mapped; catalogs, temporary and unlogged tables stay on md.c.
CREATE EXTENSION IF NOT EXISTS test_prolly;

SET debug_block_mapped_heap = on;
CREATE TABLE mapped (id int PRIMARY KEY, t text);
CREATE TEMP TABLE temp_t (id int);
CREATE UNLOGGED TABLE unlogged_t (id int);
RESET debug_block_mapped_heap;
CREATE TABLE plain (id int);

SELECT CASE WHEN c.relnamespace = 'pg_toast'::regnamespace
			THEN 'TOAST of mapped' ELSE c.relname END AS relname,
	   test_bmap_exists(c.relfilenode) AS block_mapped
FROM pg_class c
WHERE c.oid IN ('mapped'::regclass, 'mapped_pkey'::regclass,
				(SELECT reltoastrelid FROM pg_class WHERE relname = 'mapped'),
				'unlogged_t'::regclass, 'plain'::regclass, 'pg_class'::regclass)
ORDER BY 1;

-- rows written and read back, through the map
INSERT INTO mapped SELECT g, repeat('x', g % 100) FROM generate_series(1, 5000) g;
SELECT count(*), sum(length(t)) FROM mapped;
SELECT pg_relation_size('mapped') > 0 AS has_size;

-- a rewrite and a truncation keep the table block-mapped
VACUUM FULL mapped;
TRUNCATE plain;
SELECT test_bmap_exists(relfilenode) AS still_mapped FROM pg_class WHERE relname = 'mapped';
SELECT count(*) FROM mapped;

-- the page store's reserved relation file numbers are no catalog object's
SELECT count(*) AS collisions FROM pg_class
WHERE oid IN (9419, 9420, 9421, 9441, 9442)
   OR relfilenode IN (9419, 9420, 9421, 9441, 9442);

DROP TABLE mapped, temp_t, unlogged_t, plain;
SELECT entries > 0 AS drained FROM test_bmap_release_drain();

-- A dropped table's blocks are reused before the page store grows, with
-- autovacuum on as off: the extension that needs them drains the release
-- queue first.
SHOW autovacuum;
SET debug_block_mapped_heap = on;
CREATE TABLE first_t AS SELECT g AS id, repeat('y', 200) AS pad
FROM generate_series(1, 20000) g;
DROP TABLE first_t;
SELECT setting AS store_path
FROM (SELECT 'base/' || d.oid || '/9421' AS setting
	  FROM pg_database d WHERE d.datname = current_database()) s \gset
SELECT (pg_stat_file(:'store_path')).size AS store_before \gset
CREATE TABLE second_t AS SELECT g AS id, repeat('y', 200) AS pad
FROM generate_series(1, 20000) g;
-- (a block or two for the new map's nodes, not the table's 600-odd pages)
SELECT ((pg_stat_file(:'store_path')).size - :store_before) / 8192 < 16
	AS store_did_not_grow;
DROP TABLE second_t;
RESET debug_block_mapped_heap;
