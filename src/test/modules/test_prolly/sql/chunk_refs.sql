-- Chunks referencing chunks: a put counts a reference of each chunk it
-- names, and is refused if the store hasn't one; roots are taken and let
-- go of; every change of counts is its transaction's; VACUUM adds them up;
-- and the audit checks every count against the edges.
CREATE EXTENSION IF NOT EXISTS test_prolly;
SELECT test_chunk_format_refs(true);
CREATE TABLE rl () USING prolly_chunks;
CREATE TABLE ri () USING prolly_chunk_index;
CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS 'SELECT substr(sha256(d), 1, 20)';
-- a chunk of the test format: 'R', how many it names, their hashes, a tag
CREATE FUNCTION node(tag text, VARIADIC kids bytea[]) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT '\x52'::bytea || set_byte('\x00'::bytea, 0, cardinality(kids))
		|| (SELECT string_agg(h(k), ''::bytea ORDER BY o) FROM unnest(kids) WITH ORDINALITY u(k, o))
		|| convert_to(tag, 'UTF8')$$;
CREATE FUNCTION put(d bytea) RETURNS bool LANGUAGE sql
	AS $$SELECT test_chunk_put('rl', 'ri', h(d), d)$$;
CREATE FUNCTION counts(d bytea, OUT refs int8, OUT roots int8) LANGUAGE sql
	AS $$SELECT refs, roots FROM test_chunk_index_entry('ri', h(d))$$;

-- two leaves, and a node naming both
SELECT put('\x61') AS a, put('\x62') AS b;
SELECT put(node('p', '\x61', '\x62')) AS p;
SELECT 'a' AS chunk, * FROM counts('\x61') UNION ALL
SELECT 'b', * FROM counts('\x62') UNION ALL
SELECT 'p', * FROM counts(node('p', '\x61', '\x62'));
-- put again, it counts nothing again
SELECT put(node('p', '\x61', '\x62')) AS p_again, (counts('\x61')).refs AS a_refs;
-- a chunk naming one twice is two edges
SELECT put(node('q', '\x61', '\x61')) AS q, (counts('\x61')).refs AS a_refs;
-- one naming a chunk the store hasn't is refused, and not stored
SELECT put(node('m', '\x61', '\x7a7a'));
SELECT test_chunk_has('rl', 'ri', h(node('m', '\x61', '\x7a7a'))) AS m_stored,
	(counts('\x61')).refs AS a_refs;

-- a put rolled back takes its references back with it
BEGIN;
SELECT put(node('r', '\x62')) AS r, (counts('\x62')).refs AS b_refs_in;
ROLLBACK;
SELECT (counts('\x62')).refs AS b_refs_after;

-- roots: taken and let go of in a transaction, and only when it commits
BEGIN;
SELECT * FROM test_chunk_ref_root('ri', h(node('p', '\x61', '\x62')));
ROLLBACK;
SELECT (counts(node('p', '\x61', '\x62'))).roots AS p_roots_after_rollback;
SELECT * FROM test_chunk_ref_root('ri', h(node('p', '\x61', '\x62')));
SELECT * FROM test_chunk_ref_root('ri', h(node('p', '\x61', '\x62')));
SELECT * FROM test_chunk_unref_root('ri', h(node('p', '\x61', '\x62')));
SELECT * FROM test_chunk_unref_root('ri', h(node('p', '\x61', '\x62')));
SELECT * FROM test_chunk_unref_root('ri', h(node('p', '\x61', '\x62')));
SELECT * FROM test_chunk_ref_root('ri', h('\x7a7a'));

-- the audit agrees with every count
SELECT ok, chunks, unheld, why FROM test_chunk_check('rl', 'ri');

-- VACUUM adds every change into its chunk's entry: the counts the same,
-- nothing left unfrozen, the audit still agreeing
SELECT test_chunk_ref_root('ri', h(node('q', '\x61', '\x61'))) IS NOT NULL AS q_rooted;
SELECT entries AS entries_before FROM test_chunk_index_shape('ri');
VACUUM ri;
SELECT entries AS entries_after, test_chunk_index_unfrozen('ri') AS unfrozen
FROM test_chunk_index_shape('ri');
SELECT 'a' AS chunk, * FROM counts('\x61') UNION ALL
SELECT 'b', * FROM counts('\x62') UNION ALL
SELECT 'p', * FROM counts(node('p', '\x61', '\x62')) UNION ALL
SELECT 'q', * FROM counts(node('q', '\x61', '\x61'));
SELECT ok, chunks, unheld, why FROM test_chunk_check('rl', 'ri');

-- and finds a chunk naming one the store has lost
SELECT test_chunk_index_remove('ri', h('\x62')) AS removed_b;
SELECT ok, regexp_replace(why, '[0-9a-f]{40}', 'H', 'g') AS why FROM test_chunk_check('rl', 'ri');

DROP TABLE rl, ri;
DROP FUNCTION counts(bytea), put(bytea), node(text, bytea[]), h(bytea);
SELECT test_chunk_format_refs(false);
