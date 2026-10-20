-- The append check: a chunk reaches the store's files only with every
-- chunk it names, in what is appended with it or in the store.
CREATE EXTENSION IF NOT EXISTS test_prolly;

SELECT test_chunk_put('ac present') AS present \gset
SELECT test_content_hash('ac missing') AS missing \gset

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

-- A batch is checked as it is appended (a megabyte of chunks, which LZ4
-- can't make smaller), before the transaction commits.
BEGIN;
SELECT test_chunk_put_node(1, ARRAY[:'missing']) IS NOT NULL AS put;
SELECT test_chunk_put(decode(string_agg(md5('ac batch ' || g), '' ORDER BY g), 'hex'))
	IS NOT NULL AS batch FROM generate_series(1, 75000) g;
ROLLBACK;
SELECT :'LAST_ERROR_SQLSTATE' AS sqlstate;

-- Once the chunk is in the store, the node is stored.
SELECT test_chunk_put('ac missing') = :'missing' AS now_present;
SELECT test_chunk_put_node(1, ARRAY[:'present', :'missing']) = :'dangling1' AS same;
SELECT test_chunk_has(:'dangling1');

-- A subtransaction that aborts takes the chunks it put with it: one
-- naming a missing chunk fails nothing once it is rolled back.  What the
-- transaction put before, or in a subtransaction that committed, stays.
SELECT test_content_hash('ac missing in savepoint') AS missing_sp \gset
BEGIN;
SELECT test_chunk_put('ac before savepoint') AS before_sp \gset
SAVEPOINT a;
SELECT test_chunk_put('ac released') AS released \gset
RELEASE SAVEPOINT a;
SAVEPOINT b;
SELECT test_chunk_put_node(1, ARRAY[:'missing_sp']) AS rolled_back \gset
SAVEPOINT c;
SELECT test_chunk_put_node(0, ARRAY[:'missing_sp']) AS rolled_back_inner \gset
RELEASE SAVEPOINT c;
ROLLBACK TO SAVEPOINT b;
SELECT test_chunk_put('ac after savepoint') AS after_sp \gset
COMMIT;
SELECT test_chunk_has(:'before_sp') AS before_sp, test_chunk_has(:'released') AS released,
	test_chunk_has(:'after_sp') AS after_sp, test_chunk_has(:'rolled_back') AS rolled_back,
	test_chunk_has(:'rolled_back_inner') AS rolled_back_inner;
-- Likewise past a batch appended in the subtransaction: what it appended
-- stays, the rest goes.
BEGIN;
SAVEPOINT a;
SELECT test_chunk_put(decode(string_agg(md5('ac sp batch ' || g), '' ORDER BY g), 'hex'))
	AS sp_batch FROM generate_series(1, 75000) g \gset
SELECT test_chunk_put_node(1, ARRAY[:'missing_sp']) AS rolled_back2 \gset
ROLLBACK TO SAVEPOINT a;
COMMIT;
SELECT test_chunk_has(:'sp_batch') AS sp_batch, test_chunk_has(:'rolled_back2') AS rolled_back2;

-- A chunk that isn't a node names nothing.
SELECT test_chunk_has(test_chunk_put('\x01010000'::bytea));
