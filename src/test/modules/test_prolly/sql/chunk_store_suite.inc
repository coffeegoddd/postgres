-- The chunk store conformance suite, run against the store named :store
-- (chunk_store_<store>.sql).  Every statement its own transaction, so that
-- a put's pins, which last until its transaction ends, are let go of.
SELECT test_chunk_format_refs(true);
CREATE FUNCTION pg_temp.h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS 'SELECT substr(sha256(d), 1, 20)';
-- (a chunk of the test format referencing others: 'R', their count, them)
CREATE FUNCTION pg_temp.parent(VARIADIC kids bytea[]) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT 'R'::bytea || set_byte('\x00'::bytea, 0, cardinality(kids)) ||
		(SELECT string_agg(pg_temp.h(k), '' ORDER BY o) FROM unnest(kids) WITH ORDINALITY u(k, o))$$;
CREATE FUNCTION pg_temp.put(d bytea) RETURNS bool LANGUAGE sql
	AS $$SELECT test_cs_put(current_setting('test.store'), pg_temp.h(d), d)$$;
CREATE FUNCTION pg_temp.free(d bytea) RETURNS int8 LANGUAGE sql
	AS $$SELECT test_cs_free(current_setting('test.store'), pg_temp.h(d))$$;
SELECT set_config('test.store', :'store', false) IS NOT NULL AS store_set;
-- (an error's message, or none: what is refused, shown as the same in
-- every store's output)
CREATE FUNCTION pg_temp.refused(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'not refused';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLERRM;
END$$;

-- puts, has, get, counts
SELECT pg_temp.put('cs-leaf-a') AS a, pg_temp.put('cs-leaf-b') AS b,
	pg_temp.put('cs-leaf-a') AS a_again;
SELECT test_cs_has(:'store', pg_temp.h('cs-leaf-a')) AS has_a,
	test_cs_has(:'store', pg_temp.h('cs-nowhere')) AS has_nowhere,
	convert_from(test_cs_get(:'store', pg_temp.h('cs-leaf-a')), 'UTF8') AS get_a,
	test_cs_get(:'store', pg_temp.h('cs-nowhere')) IS NULL AS get_nowhere;
SELECT * FROM test_cs_counts(:'store', pg_temp.h('cs-leaf-a'));
SELECT * FROM test_cs_counts(:'store', pg_temp.h('cs-nowhere'));
-- refused: bytes that aren't their hash's, a reference to a chunk not there
SELECT pg_temp.refused($$SELECT test_cs_put(current_setting('test.store'), pg_temp.h('cs-x'), 'cs-y')$$);
SELECT pg_temp.refused($$SELECT pg_temp.put(pg_temp.parent('cs-leaf-a', 'cs-nowhere'))$$);
SELECT test_cs_has(:'store', pg_temp.h(pg_temp.parent('cs-leaf-a', 'cs-nowhere'))) AS refused_there;

-- references, counted per edge
SELECT pg_temp.put(pg_temp.parent('cs-leaf-a', 'cs-leaf-b', 'cs-leaf-b')) AS p;
SELECT (SELECT refs FROM test_cs_counts(:'store', pg_temp.h('cs-leaf-a'))) AS a_refs,
	(SELECT refs FROM test_cs_counts(:'store', pg_temp.h('cs-leaf-b'))) AS b_refs,
	(SELECT refs FROM test_cs_counts(:'store', pg_temp.h(pg_temp.parent('cs-leaf-a', 'cs-leaf-b', 'cs-leaf-b')))) AS p_refs;

-- roots; refused: of a chunk not there, and below none
SELECT test_cs_ref_root(:'store', pg_temp.h(pg_temp.parent('cs-leaf-a', 'cs-leaf-b', 'cs-leaf-b')));
SELECT roots FROM test_cs_counts(:'store', pg_temp.h(pg_temp.parent('cs-leaf-a', 'cs-leaf-b', 'cs-leaf-b')));
SELECT pg_temp.refused($$SELECT test_cs_ref_root(current_setting('test.store'), pg_temp.h('cs-nowhere'))$$);
SELECT pg_temp.refused($$SELECT test_cs_unref_root(current_setting('test.store'), pg_temp.h('cs-nowhere'))$$);
SELECT pg_temp.refused($$SELECT test_cs_unref_root(current_setting('test.store'), pg_temp.h('cs-leaf-a'))$$);

-- freeing: nothing held is freed; a chunk let go of goes, and what it alone held
SELECT pg_temp.free('cs-leaf-b') AS b_held_by_p,
	pg_temp.free(pg_temp.parent('cs-leaf-a', 'cs-leaf-b', 'cs-leaf-b')) AS p_rooted;
SELECT test_cs_unref_root(:'store', pg_temp.h(pg_temp.parent('cs-leaf-a', 'cs-leaf-b', 'cs-leaf-b')));
SELECT pg_temp.free(pg_temp.parent('cs-leaf-a', 'cs-leaf-b', 'cs-leaf-b')) AS p_and_its_leaves;
SELECT test_cs_has(:'store', pg_temp.h('cs-leaf-a')) AS has_a,
	test_cs_has(:'store', pg_temp.h('cs-leaf-b')) AS has_b;

-- a chunk is pinned until its put's transaction ends
BEGIN;
SELECT pg_temp.put('cs-leaf-d') AS d;
SELECT pg_temp.free('cs-leaf-d') AS d_pinned;
COMMIT;
SELECT pg_temp.free('cs-leaf-d') AS d_freed;
-- and found already there, too
SELECT pg_temp.put('cs-leaf-e') AS e;
BEGIN;
SELECT pg_temp.put('cs-leaf-e') AS e_again;
SELECT pg_temp.free('cs-leaf-e') AS e_pinned;
COMMIT;

-- every chunk nothing holds
SELECT test_cs_free_unheld(:'store') > 0 AS freed_some;
SELECT test_cs_has(:'store', pg_temp.h('cs-leaf-e')) AS has_e;
SELECT test_cs_check(:'store') AS audit;
SELECT test_chunk_format_refs(false);
