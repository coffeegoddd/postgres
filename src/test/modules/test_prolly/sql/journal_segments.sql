-- The journal's segments: sealing one sends appends to a new one, which
-- the manifest names after it; every segment it names is read.
CREATE EXTENSION IF NOT EXISTS test_prolly;
CREATE DATABASE segments_db;
\c segments_db
CREATE EXTENSION test_prolly;

-- A new store: segment 0 alone, once something is appended.
SELECT test_chunk_put('seg one') AS one \gset
SELECT name, chunks FROM test_chunk_segments();
SELECT test_chunk_move_root(NULL, :'one');

-- Sealed: appends go to the next segment, its id the manifest's next.
SELECT test_chunk_seal();
SELECT test_chunk_put('seg two') AS two \gset
SELECT name, chunks FROM test_chunk_segments();
SELECT convert_from(test_chunk_get(:'one'), 'UTF8') AS one,
	convert_from(test_chunk_get(:'two'), 'UTF8') AS two;
SELECT test_chunk_root() = :'one' AS root_from_sealed;
SELECT test_chunk_move_root(:'one', :'two');
SELECT test_chunk_root() = :'two' AS root_moved;

-- Another session reads them all, and the root.
\c segments_db
SELECT name, chunks FROM test_chunk_segments();
SELECT convert_from(test_chunk_get(:'one'), 'UTF8') AS one,
	convert_from(test_chunk_get(:'two'), 'UTF8') AS two;
SELECT test_chunk_root() = :'two' AS root;

-- A session open across a seal appends to the new segment.
SELECT test_chunk_seal();
SELECT test_chunk_put('seg three') AS three \gset
SELECT name, chunks FROM test_chunk_segments();

-- A transaction's batches, and its root move, across a seal.
BEGIN;
SELECT test_chunk_put(decode(string_agg(md5('seg batch ' || g), '' ORDER BY g), 'hex'))
	AS big FROM generate_series(1, 75000) g \gset
SELECT test_chunk_seal();
SELECT test_chunk_put('seg four') AS four \gset
SELECT test_chunk_move_root(:'two', :'four');
COMMIT;
SELECT name, chunks FROM test_chunk_segments();
SELECT test_chunk_root() = :'four' AS root, test_chunk_has(:'big') AS big;

-- Table files beside segments: the manifest keeps both.
SELECT test_chunk_table_file(ARRAY['seg tf'::bytea]);
SELECT name, chunks, settled FROM test_chunk_table_files();
SELECT name, chunks FROM test_chunk_segments();
\c segments_db
SELECT convert_from(test_chunk_get(:'three'), 'UTF8') AS three,
	convert_from(test_chunk_get(test_content_hash('seg tf')), 'UTF8') AS tf,
	test_chunk_root() = :'four' AS root;

-- A database made from this one gets every segment.
\c postgres
CREATE DATABASE segments_copy TEMPLATE segments_db STRATEGY wal_log;
\c segments_copy
SELECT name, chunks FROM test_chunk_segments();
SELECT convert_from(test_chunk_get(:'one'), 'UTF8') AS one,
	convert_from(test_chunk_get(:'three'), 'UTF8') AS three,
	test_chunk_root() = :'four' AS root;

-- Not in a read-only transaction.
BEGIN READ ONLY;
SELECT test_chunk_seal();
ROLLBACK;

\c postgres
DROP DATABASE segments_copy;
DROP DATABASE segments_db;
