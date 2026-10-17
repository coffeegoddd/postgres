-- Prolly tables: heap's pages in block-mapped storage, in versioned
-- schemas.  A relation is block-mapped if it has a map fork.

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

CREATE SCHEMA app VERSIONED;
CREATE TABLE app.p (id int PRIMARY KEY, t text);
CREATE TABLE h (id int PRIMARY KEY, t text);
SELECT amname FROM pg_am JOIN pg_class ON relam = pg_am.oid WHERE pg_class.oid = 'app.p'::regclass;
SELECT * FROM pg_temp.storage('app.p');
SELECT * FROM pg_temp.storage('h');

-- Rows, out-of-line values, an index scan.
INSERT INTO app.p SELECT g, repeat(md5(g::text), 100) FROM generate_series(1, 2000) g;
UPDATE app.p SET t = 'short' WHERE id % 10 = 0;
DELETE FROM app.p WHERE id % 7 = 0;
SELECT count(*), sum(length(t)) FROM app.p;
SET enable_seqscan = off;
SELECT id, length(t) FROM app.p WHERE id BETWEEN 9 AND 11 ORDER BY id;
RESET enable_seqscan;
SELECT pg_relation_size('app.p') > 0 AS has_size;

-- Storage made anew stays block-mapped.
VACUUM app.p;
VACUUM FULL app.p;
SELECT * FROM pg_temp.storage('app.p');
CLUSTER app.p USING p_pkey;
REINDEX TABLE app.p;
SELECT * FROM pg_temp.storage('app.p');
TRUNCATE app.p;
INSERT INTO app.p VALUES (1, 'one');
SELECT * FROM pg_temp.storage('app.p');
SELECT * FROM app.p;

-- Prolly tables are permanent.
CREATE UNLOGGED TABLE app.u (id int);
CREATE TEMPORARY TABLE tmp (id int) USING prolly;
ALTER TABLE app.p SET UNLOGGED;
SET default_table_access_method = prolly;
CREATE TEMPORARY TABLE tmp (id int);
CREATE UNLOGGED TABLE u2 (id int);
CREATE TABLE d (id int);
SELECT relname, amname FROM pg_class JOIN pg_am ON relam = pg_am.oid
  WHERE relname IN ('tmp', 'u2', 'd') ORDER BY 1;
RESET default_table_access_method;

-- Partitions, materialized views, CREATE TABLE AS.
CREATE TABLE app.parted (id int, v text) PARTITION BY RANGE (id);
CREATE TABLE app.parted_1 PARTITION OF app.parted FOR VALUES FROM (0) TO (100);
INSERT INTO app.parted SELECT g, 'v' FROM generate_series(0, 99) g;
SELECT * FROM pg_temp.storage('app.parted_1');
SELECT count(*) FROM app.parted;
CREATE MATERIALIZED VIEW app.mv AS SELECT id FROM app.p;
SELECT * FROM pg_temp.storage('app.mv');
REFRESH MATERIALIZED VIEW app.mv;
SELECT * FROM pg_temp.storage('app.mv');
CREATE TABLE app.ctas AS SELECT g AS id FROM generate_series(1, 100) g;
SELECT * FROM pg_temp.storage('app.ctas');
SELECT count(*) FROM app.ctas;

DROP SCHEMA app CASCADE;
DROP TABLE h, d, u2;

-- Page-based prolly has no tree engine, so no node counters: asking for
-- them is refused (as for heap), not a crash.
\set VERBOSITY sqlstate
SELECT * FROM prolly_node_counters('prolly');
SELECT * FROM prolly_node_counters('heap');
\set VERBOSITY default
