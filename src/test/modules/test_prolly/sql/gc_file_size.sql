-- A collection writes table files of a set size, prolly.gc_file_size:
-- each begun once the last holds that much of the chunks copied, as
-- Dolt's collections rotate theirs, so a store's files are about one size.
\c postgres
CREATE DATABASE file_size_db;
\c file_size_db
CREATE EXTENSION test_prolly;
-- 300 chunks of 1000 bytes each that don't compress, under one root.
CREATE TABLE live AS
SELECT g, test_chunk_put(test_noise('live ' || g, 1000)) AS hash
FROM generate_series(1, 300) g;
SELECT test_chunk_put_node(1, (SELECT array_agg(hash ORDER BY g) FROM live)) AS root \gset
SELECT test_chunk_move_root(NULL, :'root');

-- 64 kB a file: each file but the last ends with the chunk that takes it
-- past 64 kB (with its index, under 72 kB).
SET prolly.gc_file_size = '64kB';
SELECT kept FROM test_chunk_gc(true);
SELECT count(*) AS files, sum(chunks) AS chunks,
	bool_and(bytes BETWEEN 64 * 1024 AND 72 * 1024 OR name = max_name) AS each_64kb
FROM (SELECT *, max(name) OVER () AS max_name FROM test_chunk_table_files()) f;
SELECT count(*) AS live_read FROM live WHERE test_chunk_get(hash) = test_noise('live ' || g, 1000);

-- The default (64 MB): one file, for the same chunks in another store.
\c postgres
CREATE DATABASE file_size_default;
\c file_size_default
CREATE EXTENSION test_prolly;
SELECT test_chunk_move_root(NULL, test_chunk_put_node(1,
	(SELECT array_agg(test_chunk_put(test_noise('live ' || g, 1000)) ORDER BY g)
	 FROM generate_series(1, 300) g)));
SELECT kept FROM test_chunk_gc(true);
SELECT count(*) AS files, sum(chunks) AS chunks FROM test_chunk_table_files();

\c postgres
DROP DATABASE file_size_db;
DROP DATABASE file_size_default;
