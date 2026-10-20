-- A collection writes table files of a set size, prolly.gc_file_size:
-- each begun once the last's bytes (as written: compressed) reach that
-- much, as Dolt's collections rotate theirs, so a store's files are about
-- one size; a conjoin writes them so too.  The setting is the server's.
\c postgres
CREATE DATABASE file_size_db;
\c file_size_db
CREATE EXTENSION test_prolly;
SET prolly.gc_file_size = '1MB';
ALTER SYSTEM SET prolly.gc_file_size = '1MB';
SELECT pg_reload_conf();
\c file_size_db
SHOW prolly.gc_file_size;

-- 1,500 chunks of 3,000 bytes that compress well (a third of them noise),
-- under one root: some 4.5 MB as chunks, less as written where the server
-- compresses.
CREATE TABLE live AS
SELECT g, test_chunk_put(test_noise('live ' || g, 1000) ||
	convert_to(repeat('x', 2000), 'UTF8')) AS hash
FROM generate_series(1, 1500) g;
SELECT test_chunk_put_node(1, (SELECT array_agg(hash ORDER BY g) FROM live)) AS root \gset
SELECT test_chunk_move_root(NULL, :'root');

-- Each file but the last ends with the chunk that takes its bytes past
-- 1 MB (with its index, under 1.1 MB).
SELECT kept FROM test_chunk_gc(true);
SELECT count(*) > 1 AS files, sum(chunks) AS chunks,
	bool_and(bytes BETWEEN 1024 * 1024 AND 1100 * 1024 OR name = max_name) AS each_1mb
FROM (SELECT *, max(name) OVER () AS max_name FROM test_chunk_table_files()) f;
SELECT count(*) AS live_read FROM live
WHERE test_chunk_get(hash) = test_noise('live ' || g, 1000) || convert_to(repeat('x', 2000), 'UTF8');

-- Conjoined, all of them, they come out as files of the set size too.
SELECT count(test_chunk_table_file(ARRAY[test_noise('small ' || g, 100)]))
FROM generate_series(1, 5) g;
SELECT test_chunk_conjoin();
SELECT count(*) > 1 AS files, sum(chunks) AS chunks,
	bool_and(bytes BETWEEN 1024 * 1024 AND 1100 * 1024 OR name = max_name) AS each_1mb
FROM (SELECT *, max(name) OVER () AS max_name FROM test_chunk_table_files()) f;

-- The default (64 MB): one file, for the same chunks in another store.
ALTER SYSTEM RESET prolly.gc_file_size;
SELECT pg_reload_conf();
\c postgres
CREATE DATABASE file_size_default;
\c file_size_default
CREATE EXTENSION test_prolly;
SELECT test_chunk_move_root(NULL, test_chunk_put_node(1,
	(SELECT array_agg(test_chunk_put(test_noise('live ' || g, 1000) ||
		convert_to(repeat('x', 2000), 'UTF8')) ORDER BY g)
	 FROM generate_series(1, 1500) g)));
SELECT kept FROM test_chunk_gc(true);
SELECT count(*) AS files, sum(chunks) AS chunks FROM test_chunk_table_files();

\c postgres
DROP DATABASE file_size_db;
DROP DATABASE file_size_default;
