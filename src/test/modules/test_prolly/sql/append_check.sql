-- The append check: a chunk reaches the store's files only with every
-- chunk it names, in what is appended with it or in the store.
CREATE EXTENSION IF NOT EXISTS test_prolly;

SELECT test_chunk_put('ac present') AS present \gset
SELECT encode(sha256('ac missing'), 'hex') AS missing \gset

-- A node naming a chunk the store has, or one put in the same
-- transaction, is stored.
SELECT test_chunk_put_node(1, ARRAY[:'present']) AS node1 \gset
SELECT test_chunk_has(:'node1');
BEGIN;
SELECT test_chunk_put('ac new') AS new \gset
SELECT test_chunk_put_node(0, ARRAY[:'new', :'present']) AS node2 \gset
COMMIT;
SELECT test_chunk_has(:'node2');

-- One naming a chunk the store hasn't is refused, with a serialization
-- failure, whether a child or a value stored out of line: nothing of the
-- transaction is stored.
\set VERBOSITY default
BEGIN;
SELECT test_chunk_put('ac with dangling') AS with_dangling \gset
SELECT test_chunk_put_node(1, ARRAY[:'present', :'missing']) AS dangling1 \gset
COMMIT;
\set VERBOSITY terse
SELECT test_chunk_has(:'dangling1') AS dangling_stored,
	test_chunk_has(:'with_dangling') AS rest_stored;
BEGIN;
SELECT test_chunk_put_node(0, ARRAY[:'missing']) AS dangling0 \gset
COMMIT;
SELECT test_chunk_has(:'dangling0');

-- The error's code: retryable.
SELECT :'LAST_ERROR_SQLSTATE' AS sqlstate;

-- A batch is checked as it is appended (a megabyte of chunks), before the
-- transaction commits.
BEGIN;
SELECT test_chunk_put_node(1, ARRAY[:'missing']) IS NOT NULL AS put;
SELECT test_chunk_put(convert_to(repeat('ac batch', 150000), 'UTF8')) IS NOT NULL AS batch;
ROLLBACK;
SELECT :'LAST_ERROR_SQLSTATE' AS sqlstate;

-- Once the chunk is in the store, the node is stored.
SELECT test_chunk_put('ac missing') = :'missing' AS now_present;
SELECT test_chunk_put_node(1, ARRAY[:'present', :'missing']) = :'dangling1' AS same;
SELECT test_chunk_has(:'dangling1');

-- A chunk that isn't a node names nothing.
SELECT test_chunk_has(test_chunk_put('\x01010000'::bytea));
