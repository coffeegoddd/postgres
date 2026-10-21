-- Blocks past a truncated relation's length (#638): once the truncation is
-- done, a block no other map holds is freed, a shared one loses a
-- reference, and a page as of a fold goes on the changes list.  The page
-- sweep then finds nothing held past a length, and shortens the store.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE DATABASE trunc_db;
\c trunc_db
CREATE EXTENSION test_prolly;
SELECT oid AS db FROM pg_database WHERE datname = current_database() \gset
SET debug_block_mapped_heap = on;

-- A table's VACUUM truncation, its tail deleted.
CREATE TABLE h (a int, b text);
INSERT INTO h SELECT g, repeat('x', 500) FROM generate_series(1, 1500) g;
SELECT pg_relation_filenode('h') AS h \gset
SELECT pg_relation_size('h') / 8192 AS nblocks;
DELETE FROM h WHERE a > 150;
SELECT test_bmap_free_blocks() AS free_before \gset
VACUUM h;
SELECT pg_relation_size('h') / 8192 AS nblocks;
SELECT count(*) AS past_end FROM test_bmap_past_end(:h);
SELECT test_bmap_free_blocks() - :free_before AS freed;
SELECT count(*), sum(a) FROM h;

-- Nothing is left for the sweep to free; it shortens the store past them.
SELECT (pg_stat_file(format('base/%s/9421', :db))).size / 8192 AS store_before \gset
CHECKPOINT;
SELECT freed, truncated FROM prolly_page_sweep();
SELECT :store_before - (pg_stat_file(format('base/%s/9421', :db))).size / 8192
	AS store_shorter_by;

-- Growing again takes blocks anew, and reads back.
INSERT INTO h SELECT g, repeat('y', 500) FROM generate_series(1, 300) g;
SELECT count(*), sum(a) FROM h;

-- TRUNCATE in place: a table made in the same transaction.
BEGIN;
CREATE TABLE t (a int, b text);
INSERT INTO t SELECT g, repeat('x', 500) FROM generate_series(1, 600) g;
SELECT pg_relation_filenode('t') AS t \gset
SELECT test_bmap_free_blocks() AS free_before \gset
TRUNCATE t;
SELECT count(*) AS past_end FROM test_bmap_past_end(:t);
SELECT test_bmap_free_blocks() - :free_before AS freed;
COMMIT;
SELECT count(*) FROM t;

-- A change log's VACUUM truncation (the measurement's case): the entries
-- a commit's fold consumed, dead, are gone, and the log truncated.
CREATE SCHEMA v VERSIONED;
CREATE TABLE v.k (a int8 PRIMARY KEY, b text);
INSERT INTO v.k SELECT g, repeat('x', 200) FROM generate_series(1, 5000) g;
SELECT ('pg_toast.pg_prolly_log_' || 'v.k'::regclass::oid) AS logname \gset
SELECT pg_relation_filenode(:'logname') AS log \gset
SELECT pg_relation_size(:'logname') / 8192 AS log_blocks \gset
SELECT :log_blocks > 10 AS log_grown;
SELECT length(dolt_commit('-Am', 'loaded')::text) AS committed;
SELECT length(dolt_commit('--allow-empty', '-m', 'empty')::text) AS committed;
SELECT test_bmap_free_blocks() AS free_before \gset
VACUUM v.k;
SELECT pg_relation_size(:'logname') AS log_bytes;
SELECT count(*) AS past_end FROM test_bmap_past_end(:log);
SELECT test_bmap_free_blocks() - :free_before >= :log_blocks AS log_blocks_freed;

-- A shared map: the blocks a copy holds too lose a reference, and stay
-- the copy's; dropping the copy frees them.  (Cut below the table.)
CREATE TABLE s (a int, b text);
INSERT INTO s SELECT g, repeat('x', 500) FROM generate_series(1, 300) g;
SELECT pg_relation_filenode('s') AS s \gset
SELECT test_bmap_share(:s) AS copy \gset
SELECT test_bmap_digest(:copy) AS copy_digest \gset
SELECT DISTINCT refs FROM test_bmap_map(:s);
SELECT test_bmap_free_blocks() AS free_before \gset
SELECT test_bmap_truncate(:s, 5, true);
SELECT count(*) AS past_end FROM test_bmap_past_end(:s);
SELECT test_bmap_free_blocks() - :free_before AS freed;
SELECT blkno < 5 AS kept, refs, count(*) FROM test_bmap_map(:copy)
GROUP BY 1, 2 ORDER BY 1;
SELECT test_bmap_digest(:copy) = :'copy_digest' AS copy_unchanged;
SELECT test_bmap_unlink(:copy);
SELECT test_bmap_free_blocks() - :free_before AS freed_with_copy;

-- A fold: a page past the length as of the fold goes on the changes list,
-- which keeps it until a later fold's prune lets go of it.  (Cut below
-- the table.)
CREATE TABLE f (a int, b text);
INSERT INTO f SELECT g, repeat('x', 500) FROM generate_series(1, 300) g;
SELECT pg_relation_filenode('f') AS f \gset
SELECT lineage, epoch AS e1 FROM test_bmap_fold_begin(:f) \gset
SELECT phys AS phys19 FROM test_bmap_map(:f) WHERE blkno = 19 \gset
CHECKPOINT;
SELECT md5(test_bmap_read_old(:f, 19, :phys19)) AS page19 \gset
SELECT test_bmap_free_blocks() AS free_before \gset
SELECT test_bmap_truncate(:f, 5, true);
SELECT count(*) AS past_end FROM test_bmap_past_end(:f);
SELECT test_bmap_free_blocks() - :free_before AS freed;
SELECT kind, count(*), min(blkno), max(blkno)
FROM test_bmap_fold_changes(:f, :lineage, :e1) GROUP BY kind ORDER BY kind;
SELECT md5(test_bmap_read_old(:f, 19, :phys19)) = :'page19' AS page_kept;
SELECT epoch AS e2 FROM test_bmap_fold_begin(:f) \gset
SELECT test_bmap_fold_prune(:f, :lineage, :e2);
SELECT test_bmap_free_blocks() - :free_before AS freed_by_prune;
SELECT count(*), sum(a) FROM f;

CHECKPOINT;
SELECT freed FROM prolly_page_sweep();

\c postgres
DROP DATABASE trunc_db;
