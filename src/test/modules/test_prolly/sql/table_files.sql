-- Table files: chunks added together, in a file of their own, found as
-- the journal's are.
SELECT count(*) FROM test_chunk_table_files();
SELECT pg_stat_force_next_flush();
SELECT (SELECT value FROM pg_stat_prolly WHERE name = 'table_file_bytes_written') AS tf_before,
	(SELECT value FROM pg_stat_prolly WHERE name = 'manifest_bytes_written') AS mf_before \gset
SELECT test_chunk_table_file(ARRAY['tf one'::bytea, 'tf two'::bytea]);
-- (the file, and the manifest naming it, written and counted)
SELECT pg_stat_force_next_flush();
SELECT (SELECT value FROM pg_stat_prolly WHERE name = 'table_file_bytes_written') > :tf_before
		AS file_written,
	(SELECT value FROM pg_stat_prolly WHERE name = 'manifest_bytes_written') > :mf_before
		AS manifest_written;
SELECT name, chunks FROM test_chunk_table_files();
SELECT convert_from(test_chunk_get(test_content_hash('tf one'::bytea)), 'UTF8');
SELECT test_chunk_has(test_content_hash('tf two'::bytea)) AS two,
	test_chunk_has(test_content_hash('tf three'::bytea)) AS three;

-- A chunk given twice is written once.
SELECT test_chunk_table_file(ARRAY['tf dup'::bytea, 'tf dup'::bytea]);
SELECT chunks FROM test_chunk_table_files() ORDER BY name DESC LIMIT 1;

-- A table file isn't undone by its transaction's abort.
BEGIN;
SELECT test_chunk_table_file(ARRAY['tf three'::bytea]);
ROLLBACK;
SELECT test_chunk_has(test_content_hash('tf three'::bytea));
SELECT name, chunks FROM test_chunk_table_files() ORDER BY name;

-- Chunks are compressed with LZ4 where the server has it.
SELECT 'lz4' = ANY(enumvals) AS lz4 FROM pg_settings
WHERE name = 'default_toast_compression' \gset
SELECT test_chunk_table_file(array_agg(convert_to(repeat(g::text, 5000), 'UTF8')))
FROM generate_series(1, 50) g;
SELECT name, chunks,
	(bytes < (SELECT sum(length(repeat(g::text, 5000))) FROM generate_series(1, 50) g) / 10)
	= :'lz4'::bool AS compressed_if_lz4
FROM test_chunk_table_files() WHERE chunks = 50;
SELECT count(*) FROM generate_series(1, 50) g
WHERE convert_from(test_chunk_get(test_content_hash(convert_to(repeat(g::text, 5000), 'UTF8'))), 'UTF8')
	= repeat(g::text, 5000);

-- Conjoining rewrites them as one, which every session reads.
SELECT pg_stat_force_next_flush();
SELECT (SELECT value FROM pg_stat_prolly WHERE name = 'conjoin_bytes_copied') AS cj_before,
	(SELECT value FROM pg_stat_prolly WHERE name = 'table_file_bytes_written') AS tf_before \gset
SELECT test_chunk_conjoin();
-- (its file is copies of chunks the store had, and table-file bytes)
SELECT pg_stat_force_next_flush();
SELECT (SELECT value FROM pg_stat_prolly WHERE name = 'conjoin_bytes_copied') - :cj_before
	= (SELECT value FROM pg_stat_prolly WHERE name = 'table_file_bytes_written') - :tf_before
	AND (SELECT value FROM pg_stat_prolly WHERE name = 'conjoin_bytes_copied') > :cj_before
	AS conjoin_copied;
SELECT name, chunks FROM test_chunk_table_files();
SELECT count(*) FROM generate_series(1, 50) g
WHERE convert_from(test_chunk_get(test_content_hash(convert_to(repeat(g::text, 5000), 'UTF8'))), 'UTF8')
	= repeat(g::text, 5000);
\c
SELECT name, chunks FROM test_chunk_table_files();
SELECT convert_from(test_chunk_get(test_content_hash('tf three'::bytea)), 'UTF8');
SELECT test_chunk_conjoin();	-- one file: nothing to do
SELECT name, chunks FROM test_chunk_table_files();

-- Not in a read-only transaction.
BEGIN READ ONLY;
SELECT test_chunk_table_file(ARRAY['tf four'::bytea]);
ROLLBACK;

-- A file's index takes about 34 bytes a chunk read into memory (20-byte
-- hashes): each hash's 8-byte prefix and ordinal, its 12-byte suffix, its
-- offset and its filter's bits; mapped, as a store's files are, nothing.
SELECT test_tablefile_bytes_per_chunk(100000) BETWEEN 28 AND 40 AS compact_index,
	test_tablefile_bytes_per_chunk(100000, true) < 1 AS mapped_index;
SELECT test_tablefile_basic_index() AS index_checked;
