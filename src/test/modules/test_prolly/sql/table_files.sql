-- Table files: chunks added together, in a file of their own, found as
-- the journal's are.
SELECT count(*) FROM test_chunk_table_files();
SELECT test_chunk_table_file(ARRAY['tf one'::bytea, 'tf two'::bytea]);
SELECT name, chunks FROM test_chunk_table_files();
SELECT convert_from(test_chunk_get(encode(sha256('tf one'::bytea), 'hex')), 'UTF8');
SELECT test_chunk_has(encode(sha256('tf two'::bytea), 'hex')) AS two,
	test_chunk_has(encode(sha256('tf three'::bytea), 'hex')) AS three;

-- A chunk given twice is written once.
SELECT test_chunk_table_file(ARRAY['tf dup'::bytea, 'tf dup'::bytea]);
SELECT chunks FROM test_chunk_table_files() ORDER BY name DESC LIMIT 1;

-- A table file isn't undone by its transaction's abort.
BEGIN;
SELECT test_chunk_table_file(ARRAY['tf three'::bytea]);
ROLLBACK;
SELECT test_chunk_has(encode(sha256('tf three'::bytea), 'hex'));
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
WHERE convert_from(test_chunk_get(encode(sha256(convert_to(repeat(g::text, 5000), 'UTF8')), 'hex')), 'UTF8')
	= repeat(g::text, 5000);

-- Conjoining rewrites them as one, which every session reads.
SELECT test_chunk_conjoin();
SELECT name, chunks FROM test_chunk_table_files();
SELECT count(*) FROM generate_series(1, 50) g
WHERE convert_from(test_chunk_get(encode(sha256(convert_to(repeat(g::text, 5000), 'UTF8')), 'hex')), 'UTF8')
	= repeat(g::text, 5000);
\c
SELECT name, chunks FROM test_chunk_table_files();
SELECT convert_from(test_chunk_get(encode(sha256('tf three'::bytea), 'hex')), 'UTF8');
SELECT test_chunk_conjoin();	-- one file: nothing to do
SELECT name, chunks FROM test_chunk_table_files();

-- Not in a read-only transaction.
BEGIN READ ONLY;
SELECT test_chunk_table_file(ARRAY['tf four'::bytea]);
ROLLBACK;
