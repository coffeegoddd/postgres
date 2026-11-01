-- The history store's chunk log: bytes appended at its end, a chunk's bytes
-- running on across pages, read back from where they begin; a relation of
-- the prolly_chunks access method holds no tuples and refuses them.
CREATE EXTENSION IF NOT EXISTS test_prolly;
CREATE TABLE chunks () USING prolly_chunks;
SELECT test_chunk_log_end('chunks') AS empty_end;

SELECT test_chunk_log_append('chunks', '\x616263') AS first_at;
SELECT test_chunk_log_append('chunks', convert_to(repeat('0123456789', 2000), 'UTF8')) AS spanning_at;
SELECT test_chunk_log_append('chunks', '\x') AS empty_at;
SELECT test_chunk_log_append('chunks', '\x7a') AS last_at;
SELECT test_chunk_log_end('chunks') AS end_after;
SELECT pg_relation_size('chunks') / current_setting('block_size')::int AS blocks;

SELECT test_chunk_log_read('chunks', 0, 3) AS first;
SELECT test_chunk_log_read('chunks', 3, 20000) = convert_to(repeat('0123456789', 2000), 'UTF8') AS spanning_back;
SELECT test_chunk_log_read('chunks', 8000, 400) = convert_to(substr(repeat('0123456789', 2000), 7998, 400), 'UTF8') AS across_a_page;
SELECT test_chunk_log_read('chunks', 20003, 1) AS last;
SELECT test_chunk_log_read('chunks', 20004, 0) AS nothing_at_end;
SELECT test_chunk_log_read('chunks', 20003, 2);
SELECT test_chunk_log_read('chunks', 30000, 0);

-- the pages are the log's (pageinspect reads their special space)
CREATE EXTENSION IF NOT EXISTS pageinspect;
SELECT b, lower, special, pagesize FROM generate_series(0, 3) b,
	LATERAL page_header(get_raw_page('chunks', b)) ORDER BY b;

-- no tuples, and none taken
SELECT count(*) FROM chunks;
INSERT INTO chunks DEFAULT VALUES;
ALTER TABLE chunks ADD COLUMN x int;
CREATE INDEX ON chunks (x);
VACUUM chunks;
ANALYZE chunks;
VACUUM FULL chunks;
-- nor rewritten, which would leave it empty
ALTER TABLE chunks ALTER COLUMN x TYPE bigint;
ALTER TABLE chunks ADD COLUMN y float8 DEFAULT random();
ALTER TABLE chunks SET ACCESS METHOD heap;
SELECT test_chunk_log_end('chunks') AS end_kept;

-- a parallel scan and a TID scan find nothing too
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT count(*) FROM chunks;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;
SELECT count(*) FROM chunks WHERE ctid = '(1,1)';

-- appends are not transactional: an aborted one's bytes stay, the end past
-- them (what names chunks is what is transactional)
BEGIN;
SELECT test_chunk_log_append('chunks', '\x02') AS rolled_back_at;
ROLLBACK;
SELECT test_chunk_log_end('chunks') AS end_after_rollback;
-- and none in a read-only transaction
BEGIN READ ONLY;
SELECT test_chunk_log_append('chunks', '\x03');
ROLLBACK;

-- publications of all tables leave it out
SET client_min_messages = warning;
CREATE PUBLICATION chunk_pub FOR ALL TABLES;
RESET client_min_messages;
SELECT count(*) AS published FROM pg_publication_tables WHERE pubname = 'chunk_pub' AND tablename = 'chunks';
DROP PUBLICATION chunk_pub;

-- truncated, it starts again
TRUNCATE chunks;
SELECT test_chunk_log_end('chunks') AS end_truncated;
SELECT test_chunk_log_append('chunks', '\x01') AS at_after_truncate;

-- a heap is not a chunk log, nor is a chunk log anything but permanent and
-- shared
CREATE TABLE not_chunks (x int);
SELECT test_chunk_log_end('not_chunks');
CREATE UNLOGGED TABLE chunks_unlogged () USING prolly_chunks;
CREATE TEMP TABLE chunks_temp () USING prolly_chunks;
SELECT pg_branch_id_allocate() AS b \gset
SET catalog_branch = :b;
CREATE SCHEMA chunk_branch VERSIONED;
CREATE TABLE chunk_branch.chunks () USING prolly_chunks;
DROP SCHEMA chunk_branch;
RESET catalog_branch;

DROP TABLE chunks, not_chunks;
