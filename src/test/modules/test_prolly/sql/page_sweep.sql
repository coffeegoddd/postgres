-- The page sweep: what every map holds counted exactly; a block nothing
-- holds freed, a stuck count (255 maps) rewritten once it fits, the page
-- store shortened to its last allocated block.  (A dropped map's file
-- stays until a checkpoint, and counts until then.)
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE DATABASE sweep_db;
\c sweep_db
CREATE EXTENSION test_prolly;
SELECT oid AS db FROM pg_database WHERE datname = current_database() \gset

CREATE SCHEMA cow VERSIONED;
CREATE TABLE cow.t (a int8, b int8);
INSERT INTO cow.t SELECT g, g FROM generate_series(1, 1000) g;
SELECT pg_relation_filenode('cow.t') AS src \gset
SELECT pg_relation_size('cow.t') / 8192 AS nblocks \gset

-- Nothing to do in a store whose counts are right.
CHECKPOINT;
SELECT * FROM prolly_page_sweep();

-- 300 copies: the counts stick at 255.
CREATE TEMP TABLE copies AS
SELECT test_bmap_share(:src) AS copy FROM generate_series(1, 300);
SELECT DISTINCT refs FROM test_bmap_map(:src);

-- With all but 5 copies dropped, they stay stuck; a sweep counts them.
SELECT count(test_bmap_unlink(copy)) FROM copies WHERE ctid NOT IN
	(SELECT ctid FROM copies LIMIT 5);
SELECT DISTINCT refs FROM test_bmap_map(:src);
CHECKPOINT;
SELECT freed, recounted = :nblocks AS recounted FROM prolly_page_sweep();
SELECT DISTINCT refs FROM test_bmap_map(:src);

-- Dropped as usual from then on: the last holder frees them.
SELECT test_bmap_free_blocks() AS free_before \gset
SELECT count(test_bmap_unlink(copy)) FROM (SELECT copy FROM copies LIMIT 5) c;
SELECT DISTINCT refs FROM test_bmap_map(:src);
DROP TABLE cow.t;
SELECT test_bmap_free_blocks() - :free_before = :nblocks AS freed_by_drop;

-- Blocks whose holders all went while stuck stay allocated; the sweep
-- frees them, and shortens the store.
CREATE TABLE cow.u (a int8, b int8);
INSERT INTO cow.u SELECT g, g FROM generate_series(1, 1000) g;
SELECT pg_relation_filenode('cow.u') AS u \gset
CREATE TEMP TABLE ucopies AS
SELECT test_bmap_share(:u) AS copy FROM generate_series(1, 260);
SELECT count(test_bmap_unlink(copy)) FROM ucopies;
SELECT test_bmap_free_blocks() AS free_before \gset
DROP TABLE cow.u;
SELECT test_bmap_free_blocks() = :free_before AS stuck_left_allocated;
SELECT (pg_stat_file(format('base/%s/9421', :db))).size / 8192 AS store_before \gset
CHECKPOINT;
SELECT freed = :nblocks AS freed, truncated > 0 AS truncated FROM prolly_page_sweep();
SELECT (pg_stat_file(format('base/%s/9421', :db))).size / 8192 < :store_before AS store_shorter;

-- A new table reuses what was freed, and reads back.
CREATE TABLE cow.v (a int8);
INSERT INTO cow.v SELECT g FROM generate_series(1, 5000) g;
SELECT count(*), sum(a) FROM cow.v;
CHECKPOINT;
SELECT * FROM prolly_page_sweep();

-- Privileges, as dolt_gc().
CREATE ROLE sweep_user;
SET ROLE sweep_user;
SELECT * FROM prolly_page_sweep();
RESET ROLE;

\c postgres
DROP DATABASE sweep_db;
DROP ROLE sweep_user;
