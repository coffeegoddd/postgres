-- A collection's queue of chunks marked and not yet read is held to
-- maintenance_work_mem (64 kB here, 8192 of them), the rest kept in a
-- temporary file: a tree of 10,000 leaves, marked breadth first, passes
-- it, and every live chunk is still kept, the garbage freed.
\c postgres
CREATE DATABASE spill_db;
\c spill_db
CREATE EXTENSION test_prolly;
-- 100 nodes of 100 leaves each, under one root.
CREATE TABLE leaves AS
SELECT n, l, test_chunk_put(convert_to('leaf ' || n || '.' || l, 'UTF8')) AS hash
FROM generate_series(1, 100) n, generate_series(1, 100) l;
CREATE TABLE nodes AS
SELECT n, test_chunk_put_node(1, array_agg(hash ORDER BY l)) AS hash
FROM leaves GROUP BY n;
SELECT test_chunk_put_node(2, (SELECT array_agg(hash ORDER BY n) FROM nodes)) AS root \gset
SELECT test_chunk_move_root(NULL, :'root');
SELECT count(test_chunk_put(convert_to('garbage ' || g, 'UTF8')))
FROM generate_series(1, 50) g;
SELECT pg_stat_force_next_flush();
\c spill_db
SELECT temp_files AS temp_before FROM pg_stat_database WHERE datname = 'spill_db' \gset
SET maintenance_work_mem = '64kB';
SELECT kept, freed FROM test_chunk_gc(false);
SELECT pg_stat_force_next_flush();
\c spill_db
SELECT temp_files > :temp_before AS spilled FROM pg_stat_database
WHERE datname = 'spill_db';
SELECT count(*) AS leaves_kept FROM leaves WHERE test_chunk_has(hash);
SELECT test_chunk_has(test_content_hash(convert_to('garbage 1', 'UTF8'))) AS garbage_kept;

\c postgres
DROP DATABASE spill_db;
