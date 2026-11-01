-- The chunk store conformance suite, run against the store named :store
-- (chunk_store_<store>.sql).  A put's pins last until its transaction
-- ends: each step is a statement of its own, but where a transaction is
-- what is shown.
SELECT test_chunk_format_refs(true);
CREATE FUNCTION pg_temp.h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS 'SELECT substr(sha256(d), 1, 20)';
-- (a chunk of the test format referencing others: 'R', their count, them)
CREATE FUNCTION pg_temp.parent(VARIADIC kids bytea[]) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT 'R'::bytea || set_byte('\x00'::bytea, 0, cardinality(kids)) ||
		(SELECT string_agg(pg_temp.h(k), '' ORDER BY o) FROM unnest(kids) WITH ORDINALITY u(k, o))$$;
-- (a put, checked to name the chunk by its hash)
CREATE FUNCTION pg_temp.put(d bytea) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	r record;
BEGIN
	SELECT * INTO r FROM test_cs_put(current_setting('test.store'), d);
	IF r.hash <> pg_temp.h(d) THEN
		RAISE 'a put named a chunk %, not its hash', r.hash;
	END IF;
	RETURN r.stored;
END$$;
CREATE FUNCTION pg_temp.free(VARIADIC ds bytea[]) RETURNS int8 LANGUAGE sql
	AS $$SELECT test_cs_free(current_setting('test.store'), ARRAY(SELECT pg_temp.h(d) FROM unnest(ds) d))$$;
CREATE FUNCTION pg_temp.has(d bytea) RETURNS bool LANGUAGE sql
	AS $$SELECT test_cs_has(current_setting('test.store'), pg_temp.h(d))$$;
CREATE FUNCTION pg_temp.root(d bytea, ref bool) RETURNS void LANGUAGE sql
	AS $$SELECT CASE WHEN ref THEN test_cs_ref_root(current_setting('test.store'), pg_temp.h(d))
		ELSE test_cs_unref_root(current_setting('test.store'), pg_temp.h(d)) END$$;
CREATE FUNCTION pg_temp.refs(d bytea) RETURNS text LANGUAGE sql
	AS $$SELECT refs || '/' || roots FROM test_cs_counts(current_setting('test.store'), pg_temp.h(d))$$;
-- (an error's code and message, or none: what is refused, shown the same
-- in every store's output)
CREATE FUNCTION pg_temp.refused(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'not refused';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ': ' || SQLERRM;
END$$;
SELECT set_config('test.store', :'store', false) IS NOT NULL AS store_set;

-- puts, has, get, counts
SELECT pg_temp.put('cs-leaf-a') AS a, pg_temp.put('cs-leaf-b') AS b,
	pg_temp.put('cs-leaf-a') AS a_again;
SELECT pg_temp.has('cs-leaf-a') AS has_a, pg_temp.has('cs-nowhere') AS has_nowhere,
	convert_from(test_cs_get(:'store', pg_temp.h('cs-leaf-a')), 'UTF8') AS get_a,
	test_cs_get(:'store', pg_temp.h('cs-nowhere')) IS NULL AS get_nowhere;
SELECT pg_temp.refs('cs-leaf-a') AS a, pg_temp.refs('cs-nowhere') AS nowhere;
-- many at once: each where it was asked, absent ones NULL, one asked twice twice
SELECT convert_from(c, 'UTF8') AS got, o FROM unnest(test_cs_get_many(:'store',
	ARRAY[pg_temp.h('cs-leaf-b'), pg_temp.h('cs-nowhere'), pg_temp.h('cs-leaf-a'),
		  pg_temp.h('cs-leaf-b')])) WITH ORDINALITY u(c, o);
SELECT cardinality(test_cs_get_many(:'store', '{}')) AS none_asked;
SELECT pg_temp.refused($$SELECT test_cs_get_many(current_setting('test.store'), ARRAY[NULL::bytea])$$);
-- many, across pages, asked backwards: each as get has it (leaves: 'L'
-- first, no 'R')
SELECT count(*) FILTER (WHERE pg_temp.put('L'::bytea || decode(repeat(md5('cs-many-' || i), 64), 'hex')))
	AS put FROM generate_series(1, 300) i;
SELECT count(*) AS asked, count(*) FILTER (WHERE m.c = test_cs_get(:'store', pg_temp.h(d))) AS same
FROM (SELECT i, 'L'::bytea || decode(repeat(md5('cs-many-' || i), 64), 'hex') d FROM generate_series(1, 300) i) x
JOIN unnest(test_cs_get_many(:'store', ARRAY(
	SELECT pg_temp.h('L'::bytea || decode(repeat(md5('cs-many-' || i), 64), 'hex'))
	FROM generate_series(1, 300) i ORDER BY i DESC))) WITH ORDINALITY m(c, o) ON m.o = 301 - x.i;
-- refused: a reference to a chunk not there
SELECT pg_temp.refused($$SELECT pg_temp.put(pg_temp.parent('cs-leaf-a', 'cs-nowhere'))$$);
SELECT pg_temp.has(pg_temp.parent('cs-leaf-a', 'cs-nowhere')) AS refused_there;

-- references, counted per edge; the audit counting them, and the chunks
SELECT chunks AS chunks0, unheld AS unheld0 FROM test_cs_check(:'store') \gset
SELECT pg_temp.put(pg_temp.parent('cs-leaf-a', 'cs-leaf-b', 'cs-leaf-b')) AS p;
SELECT pg_temp.refs('cs-leaf-a') AS a, pg_temp.refs('cs-leaf-b') AS b,
	pg_temp.refs(pg_temp.parent('cs-leaf-a', 'cs-leaf-b', 'cs-leaf-b')) AS p;
SELECT ok, chunks - :chunks0 AS more_chunks, unheld - :unheld0 AS more_unheld, why
FROM test_cs_check(:'store');

-- roots; refused: of a chunk not there, and below none
SELECT pg_temp.root(pg_temp.parent('cs-leaf-a', 'cs-leaf-b', 'cs-leaf-b'), true);
SELECT pg_temp.refs(pg_temp.parent('cs-leaf-a', 'cs-leaf-b', 'cs-leaf-b')) AS p;
SELECT pg_temp.refused($$SELECT pg_temp.root('cs-nowhere', true)$$);
SELECT pg_temp.refused($$SELECT pg_temp.root('cs-nowhere', false)$$);
SELECT pg_temp.refused($$SELECT pg_temp.root('cs-leaf-a', false)$$);

-- freeing: nothing held is freed; a chunk let go of goes, and what it alone held
SELECT pg_temp.free('cs-leaf-b') AS b_held_by_p,
	pg_temp.free(pg_temp.parent('cs-leaf-a', 'cs-leaf-b', 'cs-leaf-b')) AS p_rooted;
SELECT pg_temp.root(pg_temp.parent('cs-leaf-a', 'cs-leaf-b', 'cs-leaf-b'), false);
SELECT pg_temp.free(pg_temp.parent('cs-leaf-a', 'cs-leaf-b', 'cs-leaf-b')) AS p_and_its_leaves;
SELECT pg_temp.has('cs-leaf-a') AS has_a, pg_temp.has('cs-leaf-b') AS has_b;
-- a leaf two parents share: freeing one leaves it
SELECT pg_temp.put('cs-shared') AS l, pg_temp.put(pg_temp.parent('cs-shared')) AS p1,
	pg_temp.put(pg_temp.parent('cs-shared', 'cs-shared')) AS p2;
SELECT pg_temp.free(pg_temp.parent('cs-shared')) AS p1_alone;
SELECT pg_temp.has('cs-shared') AS has_l, pg_temp.refs('cs-shared') AS l;
-- many at once: a parent and its leaf, one named twice, one not there
SELECT pg_temp.put('cs-pair') AS leaf, pg_temp.put(pg_temp.parent('cs-pair')) AS parent;
SELECT pg_temp.free(pg_temp.parent('cs-pair'), 'cs-pair', pg_temp.parent('cs-pair'), 'cs-nowhere')
	AS pair;

-- a pack put at once: its parent first, a chunk the store has, one twice
CREATE FUNCTION pg_temp.put_pack(VARIADIC ds bytea[]) RETURNS int8 LANGUAGE sql
	AS $$SELECT test_cs_put_many(current_setting('test.store'),
		ARRAY(SELECT pg_temp.h(d) FROM unnest(ds) WITH ORDINALITY u(d, o) ORDER BY o), ds)$$;
SELECT pg_temp.put('cs-pack-old') AS old;
SELECT pg_temp.put_pack(pg_temp.parent('cs-pack-a', 'cs-pack-b', 'cs-pack-old'), 'cs-pack-a',
	'cs-pack-b', 'cs-pack-old', 'cs-pack-a') AS stored;
SELECT pg_temp.refs('cs-pack-a') AS a, pg_temp.refs('cs-pack-b') AS b, pg_temp.refs('cs-pack-old') AS old,
	convert_from(test_cs_get(:'store', pg_temp.h('cs-pack-b')), 'UTF8') AS get_b;
SELECT test_cs_put_many(:'store', '{}', '{}') AS empty;
-- refused, and none of it put: a reference to a chunk neither the store's
-- nor the pack's, bytes not their name's
SELECT pg_temp.refused($$SELECT pg_temp.put_pack('cs-pack-c', pg_temp.parent('cs-pack-c', 'cs-nowhere'))$$);
SELECT pg_temp.has('cs-pack-c') AS c;
SELECT pg_temp.refused($$SELECT test_cs_put_many(current_setting('test.store'),
	ARRAY[pg_temp.h('cs-pack-d'), pg_temp.h('cs-pack-x')], ARRAY['cs-pack-d', 'cs-pack-y']::bytea[])$$);
SELECT pg_temp.has('cs-pack-d') AS d;
-- a pack across pages, each chunk as a put of it would be
SELECT test_cs_put_many(:'store', ARRAY(
		SELECT pg_temp.h('L'::bytea || decode(repeat(md5('cs-pack-many-' || i), 40), 'hex'))
		FROM generate_series(1, 300) i ORDER BY i),
	ARRAY(SELECT 'L'::bytea || decode(repeat(md5('cs-pack-many-' || i), 40), 'hex')
		FROM generate_series(1, 300) i ORDER BY i)) AS stored;
SELECT count(*) FILTER (WHERE test_cs_get(:'store', pg_temp.h(d)) = d) AS same
FROM (SELECT 'L'::bytea || decode(repeat(md5('cs-pack-many-' || i), 40), 'hex') d
	FROM generate_series(1, 300) i) x;
-- a pack longer than a region (in pages of its own), and one with a chunk
-- of no bytes
SELECT test_cs_put_many(:'store', ARRAY(
		SELECT pg_temp.h('L'::bytea || decode(repeat(md5('cs-pack-big-' || i), 64), 'hex'))
		FROM generate_series(1, 500) i ORDER BY i),
	ARRAY(SELECT 'L'::bytea || decode(repeat(md5('cs-pack-big-' || i), 64), 'hex')
		FROM generate_series(1, 500) i ORDER BY i)) AS stored;
SELECT count(*) FILTER (WHERE test_cs_get(:'store', pg_temp.h(d)) = d) AS same
FROM (SELECT 'L'::bytea || decode(repeat(md5('cs-pack-big-' || i), 64), 'hex') d
	FROM generate_series(1, 500) i) x;
SELECT pg_temp.put_pack('', 'cs-pack-z') AS stored;
SELECT test_cs_get(:'store', pg_temp.h('')) = '' AS empty_chunk, pg_temp.refs('') AS refs;
SELECT pg_temp.free('', 'cs-pack-z') AS freed;
-- freed as any chunks are: the pack's parent, and what it alone held (the
-- chunk the store had before too)
SELECT pg_temp.free(pg_temp.parent('cs-pack-a', 'cs-pack-b', 'cs-pack-old')) AS parent_and_three;
SELECT pg_temp.has('cs-pack-a') AS a, pg_temp.has('cs-pack-old') AS old;
SELECT ok, why FROM test_cs_check(:'store');

-- a chunk is pinned until its put's transaction ends
BEGIN;
SELECT pg_temp.put('cs-leaf-d') AS d;
SELECT pg_temp.free('cs-leaf-d') AS d_pinned;
COMMIT;
SELECT pg_temp.free('cs-leaf-d') AS d_freed;
-- and found already there, too; but not one a refused put referenced (its
-- reference, counted, gone with the refusal)
SELECT pg_temp.put('cs-leaf-e') AS e;
BEGIN;
SELECT pg_temp.put('cs-leaf-e') AS e_again;
SELECT pg_temp.free('cs-leaf-e') AS e_pinned;
COMMIT;
BEGIN;
SELECT pg_temp.refused($$SELECT pg_temp.put(pg_temp.parent('cs-leaf-e', 'cs-nowhere'))$$);
SELECT pg_temp.free('cs-leaf-e') AS e_not_held_by_refused_put;
COMMIT;

-- every chunk nothing holds: the held kept (rooted, or referenced by one
-- rooted), the rest freed
SELECT pg_temp.root(pg_temp.parent('cs-shared', 'cs-shared'), true);
SELECT pg_temp.put('cs-rooted') AS r;
SELECT pg_temp.root('cs-rooted', true);
SELECT pg_temp.put('cs-loose') AS loose;
SELECT test_cs_free_unheld(:'store') > 0 AS freed_some;
SELECT pg_temp.has('cs-rooted') AS rooted, pg_temp.has('cs-shared') AS shared,
	pg_temp.has(pg_temp.parent('cs-shared', 'cs-shared')) AS p2,
	pg_temp.has('cs-loose') AS loose, pg_temp.has('cs-leaf-e') AS e;
SELECT ok, why FROM test_cs_check(:'store');

-- a chunk found, not put, held until the transaction ends: its freeing
-- passes it by; one the store hasn't is not held
SELECT pg_temp.put('cs-held') AS held_put;
BEGIN;
SELECT test_cs_hold(:'store', pg_temp.h('cs-held')) AS held,
	test_cs_hold(:'store', pg_temp.h('cs-absent')) AS absent_held;
SELECT pg_temp.free('cs-held') AS freed_held;
COMMIT;
SELECT pg_temp.free('cs-held') AS freed_after;

-- a visit: every chunk the audit counts, with its counts
SELECT (SELECT count(*) FROM test_cs_visit(:'store')) =
	(SELECT chunks FROM test_cs_check(:'store')) AS visit_sees_all;
SELECT refs, roots FROM test_cs_visit(:'store') WHERE hash = pg_temp.h('cs-rooted');
SELECT len FROM test_cs_visit(:'store') WHERE hash = pg_temp.h('cs-shared');

-- what the store was asked, counted
SELECT 1 FROM test_cs_stats(:'store', true);
SELECT pg_temp.put('cs-stat') AS stat1, pg_temp.put('cs-stat') AS stat2;
SELECT test_cs_get(:'store', pg_temp.h('cs-stat')) IS NOT NULL AS got,
	test_cs_get(:'store', pg_temp.h('cs-nothing')) IS NULL AS absent;
SELECT test_cs_hold(:'store', pg_temp.h('cs-stat')) AS held,
	test_cs_hold(:'store', pg_temp.h('cs-nothing')) AS absent_held;
SELECT pg_temp.free('cs-stat') AS freed;
SELECT * FROM test_cs_stats(:'store', true);
-- (and of packs, many at once, and of every chunk nothing holds)
SELECT test_cs_put_many(:'store', ARRAY[pg_temp.h('cs-pack1'), pg_temp.h('cs-pack2'), pg_temp.h('cs-pack1')],
	ARRAY['cs-pack1', 'cs-pack2', 'cs-pack1']::bytea[]) AS packed;
SELECT count(*) FILTER (WHERE d IS NOT NULL) AS got_many
FROM unnest(test_cs_get_many(:'store', ARRAY[pg_temp.h('cs-pack1'), pg_temp.h('cs-nothing'), pg_temp.h('cs-pack2')])) d;
SELECT test_cs_free_unheld(:'store') >= 2 AS freed_unheld;
SELECT gets, found, puts, stored, holds, held, freed >= 2 AS freed FROM test_cs_stats(:'store', false);

-- let go of
SELECT pg_temp.root('cs-rooted', false);
SELECT pg_temp.root(pg_temp.parent('cs-shared', 'cs-shared'), false);
SELECT pg_temp.free('cs-rooted', pg_temp.parent('cs-shared', 'cs-shared')) AS rest;
SELECT test_chunk_format_refs(false);
