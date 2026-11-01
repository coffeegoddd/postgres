-- A transaction holds each chunk it puts until it ends: a chunk found
-- stored already, by a change of its counts adding nothing, written once
-- (not again by the transaction, nor a subtransaction of one that has
-- one); a pack's chunks found that another found of the pack references,
-- held by that one's references, with nothing written.
CREATE EXTENSION IF NOT EXISTS test_prolly;
SET chunk_free_in_background = off;
SELECT test_chunk_format_refs(true);
CREATE FUNCTION hh(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS 'SELECT substr(sha256(d), 1, 20)';
-- (a leaf, and a chunk referencing it)
CREATE FUNCTION leaf() RETURNS bytea LANGUAGE sql IMMUTABLE AS $$SELECT '\x0a0701'::bytea$$;
CREATE FUNCTION parent() RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT '\x5201'::bytea || hh(leaf())$$;
SELECT (test_cs_put('pages', leaf())).stored AS leaf, (test_cs_put('pages', parent())).stored AS parent;
VACUUM pg_history.chunk_index;
SELECT entries AS e0 FROM test_chunk_index_shape('pg_history.chunk_index') \gset

BEGIN;
SELECT test_cs_put_many('pages', ARRAY[hh(parent()), hh(leaf())], ARRAY[parent(), leaf()]) AS stored;
SELECT entries - :e0 AS holds_written FROM test_chunk_index_shape('pg_history.chunk_index');
SELECT test_chunk_hold('pg_history.chunk_index', hh(parent())) AS held_again;
SAVEPOINT s;
SELECT test_chunk_hold('pg_history.chunk_index', hh(parent())) AS held_in_subtransaction;
SELECT entries - :e0 AS holds_still FROM test_chunk_index_shape('pg_history.chunk_index');
RELEASE s;
-- (one of a subtransaction committed holds the chunk for the next)
SAVEPOINT a;
SELECT test_chunk_hold('pg_history.chunk_index', hh(leaf())) AS held_leaf;
RELEASE a;
SAVEPOINT b;
SELECT test_chunk_hold('pg_history.chunk_index', hh(leaf())) AS held_leaf_again;
SELECT entries - :e0 AS holds_leaf_once FROM test_chunk_index_shape('pg_history.chunk_index');
RELEASE b;
-- (its own freeing passes both by)
SELECT test_cs_free('pages', ARRAY[hh(parent()), hh(leaf())]) AS freed_own;
COMMIT;

-- a hold is a change, refused in a read-only transaction
BEGIN READ ONLY;
SELECT test_chunk_hold('pg_history.chunk_index', hh(parent()));
ROLLBACK;

-- a pack refused for a reference the store lacks counts none of the others
VACUUM pg_history.chunk_index;
SELECT entries AS e1 FROM test_chunk_index_shape('pg_history.chunk_index') \gset
SELECT test_cs_put_many('pages', ARRAY[hh('\x5202'::bytea || hh(leaf()) || hh('\x0a0799'))],
	ARRAY['\x5202'::bytea || hh(leaf()) || hh('\x0a0799')]);
SELECT entries - :e1 AS left_behind FROM test_chunk_index_shape('pg_history.chunk_index');

SELECT test_cs_free('pages', ARRAY[hh(parent()), hh(leaf())]) AS freed;
SELECT test_chunk_format_refs(false);
DROP FUNCTION hh(bytea), leaf(), parent();
