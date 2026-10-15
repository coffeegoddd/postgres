-- Prolly tables: heap's pages in block-mapped storage, beside heap tables.
-- A relation is block-mapped if it has a map fork.

CREATE FUNCTION pg_temp.block_mapped(rel regclass) RETURNS bool
LANGUAGE sql AS $$
  SELECT (pg_stat_file(pg_relation_filepath(rel) || '_bmap', true)).size IS NOT NULL
$$;

CREATE FUNCTION pg_temp.storage(tbl regclass) RETURNS TABLE (rel text, block_mapped bool)
LANGUAGE sql AS $$
  SELECT 'table', pg_temp.block_mapped(tbl)
  UNION ALL
  SELECT 'toast', pg_temp.block_mapped(reltoastrelid)
    FROM pg_class WHERE oid = tbl AND reltoastrelid <> 0
  UNION ALL
  SELECT 'index ' || indexrelid::regclass, pg_temp.block_mapped(indexrelid)
    FROM pg_index WHERE indrelid = tbl
  ORDER BY 1
$$;

CREATE TABLE p (id int PRIMARY KEY, t text) USING prolly;
CREATE TABLE h (id int PRIMARY KEY, t text);
SELECT amname FROM pg_am JOIN pg_class ON relam = pg_am.oid WHERE relname = 'p';
SELECT * FROM pg_temp.storage('p');
SELECT * FROM pg_temp.storage('h');

-- Rows, out-of-line values, an index scan.
INSERT INTO p SELECT g, repeat(md5(g::text), 100) FROM generate_series(1, 2000) g;
UPDATE p SET t = 'short' WHERE id % 10 = 0;
DELETE FROM p WHERE id % 7 = 0;
SELECT count(*), sum(length(t)) FROM p;
SET enable_seqscan = off;
SELECT id, length(t) FROM p WHERE id BETWEEN 9 AND 11 ORDER BY id;
RESET enable_seqscan;
SELECT pg_relation_size('p') > 0 AS has_size;

-- Storage made anew stays block-mapped.
VACUUM p;
VACUUM FULL p;
SELECT * FROM pg_temp.storage('p');
CLUSTER p USING p_pkey;
REINDEX TABLE p;
SELECT * FROM pg_temp.storage('p');
TRUNCATE p;
INSERT INTO p VALUES (1, 'one');
SELECT * FROM pg_temp.storage('p');
SELECT * FROM p;

-- Changing the access method moves the storage.
ALTER TABLE p SET ACCESS METHOD heap;
SELECT * FROM pg_temp.storage('p');
ALTER TABLE p SET ACCESS METHOD prolly;
SELECT * FROM pg_temp.storage('p');
SELECT * FROM p;
ALTER TABLE h SET ACCESS METHOD prolly;
SELECT * FROM pg_temp.storage('h');

-- Prolly tables are permanent.
CREATE UNLOGGED TABLE u (id int) USING prolly;
CREATE TEMPORARY TABLE tmp (id int) USING prolly;
ALTER TABLE p SET UNLOGGED;
CREATE UNLOGGED TABLE u (id int);
ALTER TABLE u SET ACCESS METHOD prolly;
SET default_table_access_method = prolly;
CREATE TEMPORARY TABLE tmp (id int);
CREATE UNLOGGED TABLE u2 (id int);
SELECT relname, amname FROM pg_class JOIN pg_am ON relam = pg_am.oid
  WHERE relname IN ('tmp', 'u2') ORDER BY 1;
CREATE TABLE d (id int);
SELECT * FROM pg_temp.storage('d');
RESET default_table_access_method;

-- Partitions, materialized views, CREATE TABLE AS.
CREATE TABLE parted (id int, v text) PARTITION BY RANGE (id) USING prolly;
CREATE TABLE parted_1 PARTITION OF parted FOR VALUES FROM (0) TO (100);
CREATE TABLE parted_2 PARTITION OF parted FOR VALUES FROM (100) TO (200) USING heap;
INSERT INTO parted SELECT g, 'v' FROM generate_series(0, 199) g;
SELECT * FROM pg_temp.storage('parted_1');
SELECT * FROM pg_temp.storage('parted_2');
SELECT count(*) FROM parted;
CREATE MATERIALIZED VIEW mv USING prolly AS SELECT id FROM p;
SELECT * FROM pg_temp.storage('mv');
REFRESH MATERIALIZED VIEW mv;
SELECT * FROM pg_temp.storage('mv');
CREATE TABLE ctas USING prolly AS SELECT g AS id FROM generate_series(1, 100) g;
SELECT * FROM pg_temp.storage('ctas');
SELECT count(*) FROM ctas;

DROP MATERIALIZED VIEW mv;
DROP TABLE p, h, d, u, parted, ctas;
