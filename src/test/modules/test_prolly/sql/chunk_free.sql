-- Freeing: a chunk nothing holds is freed, and the chunks its references
-- alone held with it, in the freeing transaction: gone if it commits,
-- all there if it aborts.  Pins hold a chunk too.  A chunk freed and put
-- again starts with nothing of the one before.
CREATE EXTENSION IF NOT EXISTS test_prolly;
SELECT test_chunk_format_refs(true);
CREATE TABLE fl () USING prolly_chunks;
CREATE TABLE fi () USING prolly_chunk_index;
CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS 'SELECT substr(sha256(d), 1, 20)';
CREATE FUNCTION node(tag text, VARIADIC kids bytea[]) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT '\x52'::bytea || set_byte('\x00'::bytea, 0, cardinality(kids))
		|| (SELECT string_agg(h(k), ''::bytea ORDER BY o) FROM unnest(kids) WITH ORDINALITY u(k, o))
		|| convert_to(tag, 'UTF8')$$;
CREATE FUNCTION put(d bytea) RETURNS bool LANGUAGE sql
	AS $$SELECT test_chunk_put('fl', 'fi', h(d), d)$$;
CREATE FUNCTION has(d bytea) RETURNS bool LANGUAGE sql
	AS $$SELECT test_chunk_has('fl', 'fi', h(d))$$;
CREATE FUNCTION free(d bytea) RETURNS int8 LANGUAGE sql
	AS $$SELECT test_chunk_free('fl', 'fi', h(d))$$;
CREATE FUNCTION tree() RETURNS bool LANGUAGE sql AS $$
	SELECT put('\x61') AND put('\x62') AND put(node('p', '\x61', '\x62'))
		AND put(node('q', node('p', '\x61', '\x62')))
		AND test_chunk_ref_root('fi', h(node('q', node('p', '\x61', '\x62')))) IS NOT NULL$$;

-- a tree, q -> p -> a, b, with a root on q: nothing of it is free
SELECT tree() AS made;
SELECT free('\x61') AS a_held, free(node('q', node('p', '\x61', '\x62'))) AS q_rooted;
-- the root let go of, freeing q frees the tree
SELECT test_chunk_unref_root('fi', h(node('q', node('p', '\x61', '\x62')))) IS NOT NULL AS unrooted;
SELECT free(node('q', node('p', '\x61', '\x62'))) AS freed;
SELECT has('\x61') AS a, has('\x62') AS b, has(node('p', '\x61', '\x62')) AS p,
	has(node('q', node('p', '\x61', '\x62'))) AS q;
SELECT ok, chunks, unheld FROM test_chunk_check('fl', 'fi');

-- freed in a transaction that rolls back, all of it is there after
SELECT tree() AS made_again;
SELECT test_chunk_unref_root('fi', h(node('q', node('p', '\x61', '\x62')))) IS NOT NULL AS unrooted;
BEGIN;
SELECT free(node('q', node('p', '\x61', '\x62'))) AS freed;
SELECT has('\x61') AS a_in;
ROLLBACK;
SELECT has('\x61') AS a, has(node('q', node('p', '\x61', '\x62'))) AS q,
	(test_chunk_index_entry('fi', h('\x61'))).refs AS a_refs;
SELECT ok, chunks, unheld FROM test_chunk_check('fl', 'fi');
SELECT free(node('q', node('p', '\x61', '\x62'))) AS freed;

-- a chunk another still names stays
SELECT put('\x61') AND put(node('r', '\x61')) AND put(node('s', '\x61')) AS made;
SELECT free(node('r', '\x61')) AS freed, (test_chunk_index_entry('fi', h('\x61'))).refs AS a_refs;
SELECT free(node('s', '\x61')) AS freed, has('\x61') AS a;

-- a pin holds a chunk until its transaction ends
SELECT put('\x7a') AS z;
BEGIN;
SELECT test_chunk_pin(h('\x7a'));
SELECT free('\x7a') AS freed_pinned;
COMMIT;
SELECT free('\x7a') AS freed;

-- freed and put again, it starts afresh: none of its old changes count
SELECT put('\x78') AS x;
SELECT test_chunk_ref_root('fi', h('\x78')) IS NOT NULL AS rooted;
VACUUM fi;
SELECT test_chunk_unref_root('fi', h('\x78')) IS NOT NULL AS unrooted;
SELECT free('\x78') AS freed;
SELECT put('\x78') AS x_again;
SELECT refs, roots FROM test_chunk_index_entry('fi', h('\x78'));

-- VACUUM takes freed chunks' entries out, and their changes
SELECT free('\x78') AS freed;
VACUUM fi;
SELECT entries, test_chunk_index_unfrozen('fi') AS unfrozen FROM test_chunk_index_shape('fi');
SELECT ok, chunks, unheld FROM test_chunk_check('fl', 'fi');

-- every chunk nothing holds, at once
SELECT tree() AS made;
SELECT put('\x63') AS c;
SELECT test_chunk_free_unheld('fl', 'fi') AS freed_unheld;
SELECT ok, chunks, unheld FROM test_chunk_check('fl', 'fi');

DROP TABLE fl, fi;
DROP FUNCTION tree(), free(bytea), has(bytea), put(bytea), node(text, bytea[]), h(bytea);
SELECT test_chunk_format_refs(false);
