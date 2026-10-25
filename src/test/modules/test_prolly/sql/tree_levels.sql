-- A tree node's level comes from its chunk, which another server may have
-- made: a node of level 31 or more (a build writes a root at 30 at most),
-- a child not one level below its parent, and an internal node of no
-- children are refused (DATA_CORRUPTED) by every reader, which keeps a
-- frame a level and descends by level.  Nodes made by hand: a leaf of one
-- item, and chains of one-child internal nodes above it.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE FUNCTION pg_temp.chain(top int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE h text := test_chunk_put_leaf(ARRAY['\x6e'::bytea]);
BEGIN
	FOR lv IN 1..top LOOP
		h := test_chunk_put_node(lv, ARRAY[h]);
	END LOOP;
	RETURN h;
END $$;

-- each reader's count, or its error's code and message
CREATE FUNCTION pg_temp.reads(root text) RETURNS TABLE (how text, result text)
LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	FOREACH how IN ARRAY ARRAY['scan', 'walk', 'diff', 'diff_nodes', 'diff_unordered',
							   'diff_estimate', 'rank', 'apply'] LOOP
		BEGIN
			result := test_tree_read(root, how)::text;
		EXCEPTION WHEN OTHERS THEN
			result := SQLSTATE || ': ' || SQLERRM;
		END;
		RETURN NEXT;
	END LOOP;
END $$;

-- a root at level 30: the tallest tree a build writes, read and edited
SELECT * FROM pg_temp.reads(pg_temp.chain(30));

-- 31: too tall
SELECT * FROM pg_temp.reads(pg_temp.chain(31));

-- a node of level 2 naming a leaf
SELECT * FROM pg_temp.reads(test_chunk_put_node(2, ARRAY[test_chunk_put_leaf(ARRAY['\x6e'::bytea])]));

-- a node of level 1 naming one of level 1
SELECT * FROM pg_temp.reads(test_chunk_put_node(1, ARRAY[pg_temp.chain(1)]));

-- a root of level 65535 naming a leaf
SELECT * FROM pg_temp.reads(test_chunk_put_node(65535, ARRAY[test_chunk_put_leaf(ARRAY['\x6e'::bytea])]));

-- an internal node of no children: as the root, and below a node of level 2
SELECT * FROM pg_temp.reads(test_chunk_put('\x010101000000000000'::bytea));
SELECT * FROM pg_temp.reads(test_chunk_put_node(2, ARRAY[test_chunk_put('\x010101000000000000'::bytea)]));

-- A value tree's leaves hold bytes and name nothing: a leaf naming a value
-- tree that names another (chunks made by hand: a leaf of one item, key
-- 'k', flagged as stored out of line, naming the chunk before) is refused
-- by a walk, as is a chain of them deep enough to overflow the stack.
CREATE FUNCTION pg_temp.ref(h text, cnt int) RETURNS bytea LANGUAGE sql AS $$
	SELECT set_byte('\x00'::bytea, 0, length(decode(h, 'hex'))) || decode(h, 'hex')
		|| set_byte('\x00'::bytea, 0, cnt) $$;
CREATE FUNCTION pg_temp.vleaf(h text) RETURNS bytea LANGUAGE sql AS $$
	SELECT '\x0101000001000000'::bytea || '\x0c000000'::bytea || '\x016b01'::bytea
		|| set_byte('\x00'::bytea, 0, length(pg_temp.ref(h, 22))) || pg_temp.ref(h, 22) $$;
CREATE FUNCTION pg_temp.nest(n int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE h text := test_chunk_put_leaf(ARRAY['\x6e'::bytea]);
BEGIN
	FOR i IN 1..n LOOP
		h := test_chunk_put(pg_temp.vleaf(h));
	END LOOP;
	RETURN h;
END $$;
-- (one: a row naming a value tree)
SELECT test_tree_read(pg_temp.nest(1), 'walk') AS walked;
SELECT * FROM pg_temp.reads(pg_temp.nest(2)) WHERE how IN ('walk', 'diff_nodes');
SELECT * FROM pg_temp.reads(pg_temp.nest(60000)) WHERE how IN ('walk', 'diff_nodes');
