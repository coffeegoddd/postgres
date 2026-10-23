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
-- (and its change log's, its index's too, which no copy holds)
SELECT (pg_relation_size(('pg_toast.pg_prolly_log_' || 'cow.t'::regclass::oid)::regclass) + pg_relation_size(('pg_toast.pg_prolly_log_' || 'cow.t'::regclass::oid || '_index')::regclass)) / 8192
	AS tlog \gset
-- (and the nodes of the maps, blocks of the page store too)
SELECT (SELECT count(DISTINCT block) FROM test_bmap_tree(pg_relation_filenode(('pg_toast.pg_prolly_log_' || 'cow.t'::regclass::oid)::regclass))) +
	(SELECT count(DISTINCT block) FROM test_bmap_tree(pg_relation_filenode(('pg_toast.pg_prolly_log_' || 'cow.t'::regclass::oid || '_index')::regclass)))
	AS tlog_nodes \gset
SELECT count(DISTINCT block) AS src_nodes FROM test_bmap_tree(:src) \gset

-- Nothing to do in a store whose counts are right.
CHECKPOINT;
SELECT * FROM prolly_page_sweep();

-- 300 copies: the counts of the subtrees they share stick at 255.
CREATE TEMP TABLE copies AS
SELECT test_bmap_share(:src) AS copy FROM generate_series(1, 300);
SELECT DISTINCT refs FROM test_bmap_map(:src);

-- With all but 5 copies dropped, they stay stuck; a sweep counts them.
SELECT count(test_bmap_unlink(copy)) FROM copies WHERE ctid NOT IN
	(SELECT ctid FROM copies LIMIT 5);
SELECT DISTINCT refs FROM test_bmap_map(:src);
CHECKPOINT;
SELECT count(*) AS stuck FROM test_bmap_tree(:src) WHERE refs = 255 \gset
SELECT freed, recounted = :stuck AS recounted FROM prolly_page_sweep();
SELECT DISTINCT refs FROM test_bmap_map(:src);

-- Dropped as usual from then on: the last holder frees them.
SELECT test_bmap_free_blocks() AS free_before \gset
SELECT count(test_bmap_unlink(copy)) FROM (SELECT copy FROM copies LIMIT 5) c;
SELECT DISTINCT refs FROM test_bmap_map(:src);
DROP TABLE cow.t;
SELECT entries > 0 AS drained FROM test_bmap_release_drain();
SELECT test_bmap_free_blocks() - :free_before =
	:nblocks + :tlog + :tlog_nodes + :src_nodes AS freed_by_drop;

-- Blocks whose holders all went while stuck stay allocated; the sweep
-- frees them, and shortens the store.
CREATE TABLE cow.u (a int8, b int8);
INSERT INTO cow.u SELECT g, g FROM generate_series(1, 1000) g;
SELECT pg_relation_filenode('cow.u') AS u \gset
SELECT (pg_relation_size(('pg_toast.pg_prolly_log_' || 'cow.u'::regclass::oid)::regclass) + pg_relation_size(('pg_toast.pg_prolly_log_' || 'cow.u'::regclass::oid || '_index')::regclass)) / 8192
	AS ulog \gset
SELECT (SELECT count(DISTINCT block) FROM test_bmap_tree(pg_relation_filenode(('pg_toast.pg_prolly_log_' || 'cow.u'::regclass::oid)::regclass))) +
	(SELECT count(DISTINCT block) FROM test_bmap_tree(pg_relation_filenode(('pg_toast.pg_prolly_log_' || 'cow.u'::regclass::oid || '_index')::regclass)))
	AS ulog_nodes \gset
SELECT count(DISTINCT block) AS u_nodes FROM test_bmap_tree(:u) \gset
CREATE TEMP TABLE ucopies AS
SELECT test_bmap_share(:u) AS copy FROM generate_series(1, 260);
SELECT count(test_bmap_unlink(copy)) FROM ucopies;
SELECT test_bmap_free_blocks() AS free_before \gset
DROP TABLE cow.u;
SELECT entries > 0 AS drained FROM test_bmap_release_drain();
SELECT test_bmap_free_blocks() - :free_before = :ulog + :ulog_nodes AS stuck_left_allocated;
SELECT (pg_stat_file(format('base/%s/9421', :db))).size / 8192 AS store_before \gset
CHECKPOINT;
SELECT freed = :nblocks + :u_nodes AS freed, truncated > 0 AS truncated FROM prolly_page_sweep();
SELECT (pg_stat_file(format('base/%s/9421', :db))).size / 8192 < :store_before AS store_shorter;

-- A new table reuses what was freed, and reads back.
CREATE TABLE cow.v (a int8);
INSERT INTO cow.v SELECT g FROM generate_series(1, 5000) g;
SELECT count(*), sum(a) FROM cow.v;
CHECKPOINT;
SELECT * FROM prolly_page_sweep();

-- A sweep is due when a stuck count loses a holder (it keeps it, stuck),
-- not for counts left stuck by the last sweep, their holders all there.
CREATE TABLE cow.z (a int8);
INSERT INTO cow.z SELECT g FROM generate_series(1, 1000) g;
SELECT pg_relation_filenode('cow.z') AS z \gset
CREATE TEMP TABLE zcopies AS
SELECT test_bmap_share(:z) AS copy FROM generate_series(1, 300);
CHECKPOINT;
SELECT freed >= 0 AS swept FROM prolly_page_sweep();
SELECT test_bmap_sweep_due() AS due_after_sweep;
SELECT count(test_bmap_unlink(copy)) FROM (SELECT copy FROM zcopies LIMIT 10) c;
SELECT test_bmap_sweep_due() AS due_after_holders_left;
SELECT recounted = 0 AS still_stuck FROM prolly_page_sweep();
SELECT test_bmap_sweep_due() AS due_after_second_sweep;

-- Privileges, as dolt_gc().
CREATE ROLE sweep_user;
SET ROLE sweep_user;
SELECT * FROM prolly_page_sweep();
RESET ROLE;

\c postgres
DROP DATABASE sweep_db;
DROP ROLE sweep_user;
