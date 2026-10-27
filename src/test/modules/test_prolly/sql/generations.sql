-- Generations of the chunk store: a collection that settles chunks gives
-- the files it writes its generation (the store's GC generation it
-- begins), recorded with when it ended; files not settled (a transfer's)
-- have none.  A conjoin of settled files and a new session keep them.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE DATABASE generations_db;
\c generations_db
CREATE EXTENSION test_prolly;

SELECT test_chunk_put('gen a') AS a \gset
SELECT test_chunk_put_node(1, ARRAY[:'a']) AS n1 \gset
SELECT test_chunk_move_root(NULL, :'n1');
SELECT kept, freed FROM test_chunk_gc(false);
SELECT test_chunk_generation() AS g1 \gset
SELECT chunks, settled, epoch_lo = :g1 AS lo, epoch_hi = :g1 AS hi
	FROM test_chunk_generations();
SELECT epoch = :g1 AS epoch, ended <= now() AS ended FROM test_chunk_epochs();

-- A transfer's file: no generation, until a collection settles it.
SELECT test_chunk_table_file(ARRAY['gen tf'::bytea]);
SELECT test_chunk_put_node(2, ARRAY[:'n1', test_content_hash('gen tf')]) AS n2 \gset
SELECT test_chunk_move_root(:'n1', :'n2');
SELECT chunks, settled, epoch_lo, epoch_hi FROM test_chunk_generations()
	ORDER BY settled, epoch_lo;
SELECT kept, freed FROM test_chunk_gc(false);
SELECT test_chunk_generation() AS g2 \gset
SELECT chunks, settled, epoch_lo = :g1 AS g1, epoch_lo = :g2 AS g2
	FROM test_chunk_generations() ORDER BY epoch_lo;
SELECT count(*) AS epochs, min(epoch) = :g1 AS g1, max(epoch) = :g2 AS g2
	FROM test_chunk_epochs();

-- A conjoin of the settled files spans their generations.
SELECT test_chunk_conjoin();
SELECT chunks, settled, epoch_lo = :g1 AS lo, epoch_hi = :g2 AS hi
	FROM test_chunk_generations();

-- Another session reads the same.
\c generations_db
SELECT settled, epoch_lo = :g1 AS lo, epoch_hi = :g2 AS hi
	FROM test_chunk_generations() ORDER BY settled;

\c postgres
DROP DATABASE generations_db;
