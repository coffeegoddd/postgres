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

-- The newer file's node names the older's: listed in its section.
SELECT epoch_lo = :g2 AS g2, xrefs FROM test_chunk_generations() ORDER BY epoch_lo;
SELECT test_chunk_verify_generations() AS checked;

-- A collection that settles nothing new records no generation.
SELECT kept, freed FROM test_chunk_gc(false);
SELECT count(*) AS epochs FROM test_chunk_epochs();

-- A full collection keeps each chunk's generation.  A third file of a
-- node and three leaves; then the root moves to a node of its own naming
-- one of the leaves, the rest garbage: the third file, mostly garbage, is
-- rewritten with its live leaf kept in the third generation; the first
-- two are dropped; the new node is the collection's, and names the third
-- generation's leaf.
SELECT test_chunk_put('gen c') AS c \gset
SELECT test_chunk_put_node(3, ARRAY[:'c', test_chunk_put('gen d'),
	test_chunk_put('gen e')]) AS n3 \gset
SELECT test_chunk_move_root(:'n2', :'n3');
SELECT kept, freed FROM test_chunk_gc(false);
SELECT test_chunk_generation() AS g3 \gset
SELECT test_chunk_put_node(1, ARRAY[:'c']) AS n5 \gset
SELECT test_chunk_move_root(:'n3', :'n5');
SELECT kept, freed FROM test_chunk_gc(true);
SELECT test_chunk_generation() AS g4 \gset
SELECT chunks, epoch_lo = :g3 AS g3, epoch_lo = :g4 AS g4, xrefs
	FROM test_chunk_generations() ORDER BY epoch_lo;
SELECT test_chunk_has(:'c') AS c, test_chunk_has(:'n5') AS n5,
	test_chunk_has(:'a') AS a, test_chunk_has(:'n3') AS n3;
SELECT test_chunk_verify_generations() AS checked;

-- A conjoin takes settled files in a run by generation: of the files of
-- one, one, twenty-one and one chunks, the first two, not the three of
-- one chunk (which would span the third's generation).
SELECT test_chunk_put_node(2, ARRAY(SELECT test_chunk_put(('gen big ' || g)::bytea)
	FROM generate_series(1, 20) g) || :'n5'::text) AS big \gset
SELECT test_chunk_move_root(:'n5', :'big');
SELECT kept, freed FROM test_chunk_gc(false);
SELECT test_chunk_put_node(3, ARRAY[:'big']) AS n6 \gset
SELECT test_chunk_move_root(:'big', :'n6');
SELECT kept, freed FROM test_chunk_gc(false);
SELECT chunks, epoch_lo - :g3 AS lo, epoch_hi - :g3 AS hi
	FROM test_chunk_generations() ORDER BY epoch_lo;
SELECT test_chunk_conjoin(3);
SELECT chunks, epoch_lo - :g3 AS lo, epoch_hi - :g3 AS hi, xrefs
	FROM test_chunk_generations() ORDER BY epoch_lo;
SELECT test_chunk_verify_generations() AS checked;

-- All of them conjoined span them all, and list nothing.
SELECT test_chunk_conjoin();
SELECT chunks, epoch_lo = :g3 AS lo, xrefs FROM test_chunk_generations();
SELECT test_chunk_verify_generations() AS checked;

-- Another session reads the same.
\c generations_db
SELECT settled, epoch_lo = :g3 AS lo FROM test_chunk_generations();

\c postgres
DROP DATABASE generations_db;
