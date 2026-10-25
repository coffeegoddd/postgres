-- jsonb's operators on a versioned table's long documents: ->, ->>, #>,
-- #>>, ? and jsonb_set() read the path from the document's JSON tree, or
-- set it by editing the tree, and give what they give on the document
-- in a heap table, on random documents and random paths.
CREATE DATABASE regress_jsonb_tree_ops ENCODING 'UTF8' LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0;
\c regress_jsonb_tree_ops
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;

CREATE SCHEMA vt VERSIONED;
CREATE TABLE vt.docs (id int PRIMARY KEY, j jsonb);
CREATE TABLE plain (id int PRIMARY KEY, j jsonb);

-- A random value: scalars of every kind (JSON null, numerics of several
-- scales, strings short and long), objects whose keys are names or look
-- like indexes, arrays.
CREATE FUNCTION rand_value(depth int, width int) RETURNS jsonb
LANGUAGE plpgsql AS $$
DECLARE
	r float8 := random();
	o jsonb;
BEGIN
	IF depth <= 0 OR r < 0.3 THEN
		r := random();
		RETURN CASE
			WHEN r < 0.15 THEN 'null'::jsonb
			WHEN r < 0.3 THEN to_jsonb(random() < 0.5)
			WHEN r < 0.55 THEN to_jsonb(round((random() * 2000 - 1000)::numeric, (random() * 3)::int))
			WHEN r < 0.95 THEN to_jsonb(md5(random()::text))
			ELSE to_jsonb(repeat('long ', (random() * 600)::int)) END;
	END IF;
	IF random() < 0.5 THEN
		o := '{}';
		FOR i IN 1..(random() * width)::int LOOP
			o := o || jsonb_build_object(CASE WHEN random() < 0.6 THEN 'k' || (random() * 40)::int
										  ELSE ((random() * 12)::int)::text END,
										 rand_value(depth - 1, width));
		END LOOP;
	ELSE
		o := '[]';
		FOR i IN 1..(random() * width)::int LOOP
			o := o || jsonb_build_array(rand_value(depth - 1, width));
		END LOOP;
	END IF;
	RETURN o;
END $$;

-- Documents that are objects and arrays, long enough to be trees, with
-- long arrays and objects (their elements items of their own) and short
-- ones (packed).
SELECT setseed(0.975);
INSERT INTO plain
SELECT g, CASE WHEN g % 3 = 0 THEN
		(SELECT jsonb_agg(rand_value(3, 6)) FROM generate_series(1, 40))
	ELSE
		(SELECT jsonb_object_agg(CASE WHEN h % 4 = 0 THEN h::text ELSE 'k' || h END, rand_value(3, 6))
		 FROM generate_series(1, 40) h) ||
		jsonb_build_object('long', (SELECT jsonb_agg(md5(h::text)) FROM generate_series(1, 100) h))
	END
FROM generate_series(1, 9) g;
INSERT INTO vt.docs SELECT * FROM plain;
SELECT count(*) AS docs, count(*) FILTER (WHERE (test_toast_pointer(j)).form = 3) AS trees
FROM vt.docs;

-- Every path in the documents, to a depth of 5.
CREATE TEMP TABLE all_paths AS
WITH RECURSIVE t(id, path, v) AS (
	SELECT id, '{}'::text[], j FROM plain
	UNION ALL
	SELECT t.id, t.path || e.k, e.v
	FROM t, LATERAL (
		SELECT key AS k, value AS v
		FROM jsonb_each(CASE WHEN jsonb_typeof(t.v) = 'object' THEN t.v ELSE '{}' END)
		UNION ALL
		SELECT (ord - 1)::text, value
		FROM jsonb_array_elements(CASE WHEN jsonb_typeof(t.v) = 'array' THEN t.v ELSE '[]' END)
			WITH ORDINALITY AS a(value, ord)) e
	WHERE cardinality(t.path) < 5)
SELECT id, path, jsonb_typeof(v) AS kind,
	   CASE jsonb_typeof(v) WHEN 'array' THEN jsonb_array_length(v) END AS len
FROM t;

-- A sample of them, each also changed: its last step missing, negative
-- (in range, at the start, past it), past an array's end, not an integer,
-- NULL, signed, INT_MIN; a step past it; its first step alone.
CREATE TEMP TABLE paths (id int, path text[]);
INSERT INTO paths
SELECT s.id, v.path
FROM (SELECT * FROM all_paths WHERE cardinality(path) > 0 ORDER BY random() LIMIT 400) s,
	 LATERAL (VALUES
		(s.path),
		(s.path[1:cardinality(s.path) - 1] || 'missing'::text),
		(s.path[1:cardinality(s.path) - 1] || '-1'::text),
		(s.path[1:cardinality(s.path) - 1] || (-1 - (random() * 30)::int)::text),
		(s.path[1:cardinality(s.path) - 1] || '999'::text),
		(s.path[1:cardinality(s.path) - 1] || 'x1'::text),
		(s.path[1:cardinality(s.path) - 1] || NULL::text),
		(s.path[1:cardinality(s.path) - 1] || '+1'::text),
		(s.path[1:cardinality(s.path) - 1] || '-2147483648'::text),
		(s.path || '0'::text),
		(s.path || 'k1'::text),
		(s.path[1:1])) AS v(path);
-- a negative index for every long array's every element and one past
INSERT INTO paths
SELECT a.id, a.path || (-i)::text
FROM (SELECT * FROM all_paths WHERE kind = 'array' AND cardinality(path) < 2) a,
	 generate_series(1, a.len + 1) i;
INSERT INTO paths SELECT id, '{}' FROM plain;
SELECT count(*) > 4000 AS many_paths FROM paths;

-- Reads: each operator gives what it gives on the heap table's document.
-- Rows listed are paths where they differ (none).
SELECT p.id, p.path, op
FROM paths p JOIN vt.docs d USING (id) JOIN plain h USING (id),
	 LATERAL (VALUES
		('#>', (d.j #> p.path) IS NOT DISTINCT FROM (h.j #> p.path)),
		('#>>', (d.j #>> p.path) IS NOT DISTINCT FROM (h.j #>> p.path)),
		('->', (d.j -> p.path[1]) IS NOT DISTINCT FROM (h.j -> p.path[1])),
		('->>', (d.j ->> p.path[1]) IS NOT DISTINCT FROM (h.j ->> p.path[1])),
		('-> int', CASE WHEN p.path[1] ~ '^-?[0-9]{1,9}$' THEN
			(d.j -> p.path[1]::int) IS NOT DISTINCT FROM (h.j -> p.path[1]::int) ELSE true END),
		('->> int', CASE WHEN p.path[1] ~ '^-?[0-9]{1,9}$' THEN
			(d.j ->> p.path[1]::int) IS NOT DISTINCT FROM (h.j ->> p.path[1]::int) ELSE true END),
		('?', (d.j ? p.path[1]) IS NOT DISTINCT FROM (h.j ? p.path[1])),
		('jsonb_extract_path', jsonb_extract_path(d.j, VARIADIC p.path) IS NOT DISTINCT FROM
			jsonb_extract_path(h.j, VARIADIC p.path)),
		('jsonb_extract_path_text', jsonb_extract_path_text(d.j, VARIADIC p.path) IS NOT DISTINCT FROM
			jsonb_extract_path_text(h.j, VARIADIC p.path))) AS r(op, same)
WHERE NOT same
ORDER BY 1, 2, 3;
-- INT_MIN as -> takes it, and the last element
SELECT d.id, (d.j -> (-2147483648)) IS NOT DISTINCT FROM (h.j -> (-2147483648)) AS int_min,
	   (d.j -> (-1)) IS NOT DISTINCT FROM (h.j -> (-1)) AS last,
	   (d.j ->> (-1)) IS NOT DISTINCT FROM (h.j ->> (-1)) AS last_text
FROM vt.docs d JOIN plain h USING (id) WHERE d.id % 3 = 0 ORDER BY 1;

-- jsonb_set(), creating and not, with values of every kind (one long
-- enough to make a packed container one no longer): the documents it
-- gives, or its errors (an index that isn't an integer, a NULL step, a
-- scalar document).
CREATE FUNCTION try_set(j jsonb, path text[], v jsonb, c bool) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
	RETURN jsonb_set(j, path, v, c)::text;
EXCEPTION WHEN OTHERS THEN
	RETURN 'error: ' || SQLERRM;
END $$;
CREATE TEMP TABLE set_values (n int, v jsonb);
INSERT INTO set_values VALUES (0, '"set"'), (1, 'null'), (2, '{"n": [1, 2.50]}'), (3, '[]'),
	(4, to_jsonb(repeat('grown ', 300))), (5, '-0.000');
CREATE TEMP TABLE set_cases AS
SELECT p.id, p.path, (random() * 5)::int AS n, random() < 0.5 AS c
FROM paths p WHERE random() < 0.3;
-- (the heap's document tells which are errors: try_set's subtransaction
-- takes no tree edit, so the others are compared outside one)
ALTER TABLE set_cases ADD COLUMN err bool;
UPDATE set_cases s SET err = try_set(h.j, s.path, v.v, s.c) LIKE 'error:%'
FROM plain h, set_values v WHERE h.id = s.id AND v.n = s.n;
SELECT count(*) > 1000 AS many_sets, count(*) FILTER (WHERE err) > 0 AS errors
FROM set_cases;
WITH ok AS MATERIALIZED (SELECT * FROM set_cases WHERE NOT err)
SELECT s.id, s.path, s.n, s.c
FROM ok s JOIN set_values USING (n) JOIN vt.docs d USING (id) JOIN plain h USING (id)
WHERE jsonb_set(d.j, s.path, v, s.c)::text IS DISTINCT FROM jsonb_set(h.j, s.path, v, s.c)::text
ORDER BY 1, 2, 3, 4;
-- the errors, each statement on its own (a tree edit would give a row)
SELECT format('SELECT jsonb_set(j, %L, %L, %L) FROM vt.docs WHERE id = %s',
			  s.path, v.v, s.c, s.id)
FROM (SELECT * FROM set_cases WHERE err AND n <> 4 ORDER BY id, path::text, n, c LIMIT 12) s
	 JOIN set_values v USING (n)
ORDER BY s.id, s.path::text, s.n, s.c \gexec
-- a path of more than one dimension: jsonb_set's error
SELECT jsonb_set(j, '{{a},{b}}', '1') FROM vt.docs WHERE id = 1;

-- A sequence of edits by UPDATE: each step gives what the heap table's
-- does, and the tree's root is the root of the document built whole.
CREATE TEMP TABLE edit_results (step int, same bool, same_root bool);
DO $$
DECLARE
	e record;
	r1 text;
	r2 text;
	k int := 0;
BEGIN
	FOR e IN SELECT s.*, v FROM set_cases s JOIN set_values USING (n)
			 WHERE s.path IS NOT NULL AND array_position(s.path, NULL) IS NULL
				   AND NOT s.err
			 ORDER BY random() LIMIT 60 LOOP
		k := k + 1;
		UPDATE vt.docs SET j = jsonb_set(j, e.path, e.v, e.c) WHERE id = e.id;
		UPDATE plain SET j = jsonb_set(j, e.path, e.v, e.c) WHERE id = e.id;
		DELETE FROM vt.docs WHERE id = 100;
		INSERT INTO vt.docs SELECT 100, j FROM plain WHERE id = e.id;
		SELECT prolly_jsonb_tree_root(j) INTO r1 FROM vt.docs WHERE id = e.id;
		SELECT prolly_jsonb_tree_root(j) INTO r2 FROM vt.docs WHERE id = 100;
		INSERT INTO edit_results
		SELECT k, d.j = h.j, r1 IS NOT DISTINCT FROM r2
		FROM vt.docs d JOIN plain h USING (id) WHERE id = e.id;
	END LOOP;
	DELETE FROM vt.docs WHERE id = 100;
END $$;
SELECT count(*) AS steps, count(*) FILTER (WHERE same AND same_root) AS same FROM edit_results;

-- Each operator reads the nodes on its path, not the document's: fewer
-- than a tenth of the nodes a whole read reads, and so does an UPDATE by
-- jsonb_set().
CREATE TABLE vt.big (id int PRIMARY KEY, j jsonb);
INSERT INTO vt.big
SELECT 1, jsonb_build_object('items', jsonb_object_agg('k' || g, jsonb_build_object('id', g, 'name', md5(g::text))),
							 'meta', '{"n": 1}'::jsonb, 'arr', (SELECT jsonb_agg(md5(h::text)) FROM generate_series(1, 2000) h))
FROM generate_series(1, 20000) g;
INSERT INTO vt.big SELECT 2, jsonb_agg(md5(g::text)) FROM generate_series(1, 20000) g;
SELECT test_toast_counters(true) IS NOT NULL AS reset;
SELECT length(j::text) > 0 AS read_whole FROM vt.big WHERE id = 1;
SELECT (test_toast_counters(true)).nodes_read AS whole_nodes \gset
CREATE TEMP TABLE reads (op text, nodes bigint);
SELECT j -> 'meta' FROM vt.big WHERE id = 1 \g /dev/null
INSERT INTO reads SELECT '->', (test_toast_counters(true)).nodes_read;
SELECT j ->> 'arr' IS NOT NULL FROM vt.big WHERE id = 1 \g /dev/null
INSERT INTO reads SELECT '->>', (test_toast_counters(true)).nodes_read;
SELECT j #> '{items,k77,name}' FROM vt.big WHERE id = 1 \g /dev/null
INSERT INTO reads SELECT '#>', (test_toast_counters(true)).nodes_read;
SELECT j #>> '{items,k77,name}' FROM vt.big WHERE id = 1 \g /dev/null
INSERT INTO reads SELECT '#>>', (test_toast_counters(true)).nodes_read;
SELECT j #> '{arr,-3}' FROM vt.big WHERE id = 1 \g /dev/null
INSERT INTO reads SELECT '#> negative', (test_toast_counters(true)).nodes_read;
SELECT j ? 'arr' FROM vt.big WHERE id = 1 \g /dev/null
INSERT INTO reads SELECT '?', (test_toast_counters(true)).nodes_read;
SELECT op, nodes > 0 AND nodes * 10 < :whole_nodes AS path_reads_less FROM reads ORDER BY op;
SELECT length(j::text) > 0 AS read_whole FROM vt.big WHERE id = 2;
SELECT (test_toast_counters(true)).nodes_read AS whole_array_nodes \gset
SELECT j -> 1500, j ->> (-1) FROM vt.big WHERE id = 2 \g /dev/null
SELECT (test_toast_counters(true)).nodes_read AS index_nodes \gset
SELECT :index_nodes > 0 AND :index_nodes * 10 < :whole_array_nodes AS index_reads_less;
SELECT j #>> '{items,k77,name}' AS name, j -> 'meta' AS meta, j ->> 'arr' = (j -> 'arr')::text AS arr, j #> '{arr,-3}' AS third_last
FROM vt.big WHERE id = 1;
SELECT j -> 1500 AS elem, j ->> (-1) AS last FROM vt.big WHERE id = 2;
SELECT test_toast_counters(true) IS NOT NULL AS reset;
UPDATE vt.big SET j = jsonb_set(j, '{items,k77,name}', '"edited"') WHERE id = 1;
SELECT (test_toast_counters(true)).nodes_read AS set_nodes \gset
SELECT :set_nodes > 0 AND :set_nodes * 10 < :whole_nodes AS set_reads_less;
SELECT j #>> '{items,k77,name}' AS edited, (test_toast_pointer(j)).form AS form FROM vt.big WHERE id = 1;

-- No tree edit where its nodes wouldn't outlive the result, or can't be
-- written: jsonb_set() edits the document whole, giving what it gives.
-- In a read-only transaction, taking no transaction ID:
BEGIN READ ONLY;
SELECT jsonb_set(j, '{items,k78,name}', '"read only"') #>> '{items,k78,name}' AS set,
	   (test_toast_pointer(jsonb_set(j, '{items,k78,name}', '"read only"'))) IS NULL AS whole
FROM vt.big WHERE id = 1;
SELECT pg_current_xact_id_if_assigned() IS NULL AS no_xid;
COMMIT;
-- In a subtransaction that aborts, the result living on in a variable:
CREATE FUNCTION set_in_subxact() RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	x jsonb;
BEGIN
	BEGIN
		SELECT jsonb_set(j, '{items,k5}', '"in subxact"') INTO x FROM vt.big WHERE id = 1;
		RAISE EXCEPTION 'boom';
	EXCEPTION WHEN OTHERS THEN
		NULL;
	END;
	INSERT INTO vt.big VALUES (4, x);
	RETURN x #>> '{items,k5}';
END $$;
SELECT set_in_subxact();
SELECT j #>> '{items,k5}' AS kept, (SELECT j FROM vt.big WHERE id = 4) =
	   jsonb_set((SELECT j FROM vt.big WHERE id = 1), '{items,k5}', '"in subxact"') AS same
FROM vt.big WHERE id = 4;
-- In a parallel plan, its workers' edits: a worker's chunks would be lost
-- with it, and it can take no transaction ID.
CREATE TABLE vt.many (id int PRIMARY KEY, j jsonb);
INSERT INTO vt.many
SELECT i, (SELECT jsonb_object_agg('k' || g, md5((i * 1000 + g)::text)) FROM generate_series(1, 100) g)
FROM generate_series(1, 300) i;
SELECT count(*) FILTER (WHERE (test_toast_pointer(j)).form = 3) AS trees FROM vt.many;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM vt.many WHERE jsonb_set(j, '{k5}', '"x"') ->> 'k5' = 'x';
SELECT count(*) FROM vt.many WHERE jsonb_set(j, '{k5}', '"x"') ->> 'k5' = 'x';
BEGIN;
INSERT INTO vt.many VALUES (1000, '{}');
CREATE TEMP TABLE many_copy AS SELECT id, jsonb_set(j, '{k5}', '"x"') AS j FROM vt.many WHERE id < 1000;
SELECT count(*) FILTER (WHERE j ->> 'k5' = 'x' AND j ->> 'k6' IS NOT NULL) AS edited FROM many_copy;
COMMIT;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;
-- and a plan run in a worker by debug_parallel_query
SET debug_parallel_query = on;
BEGIN;
INSERT INTO vt.many VALUES (1001, '{}');
CREATE TEMP TABLE forced_copy AS SELECT id, jsonb_set(j, '{k7}', '"y"') AS j FROM vt.many WHERE id = 7;
SELECT j ->> 'k7' AS k7, j ->> 'k8' IS NOT NULL AS rest FROM forced_copy;
COMMIT;
RESET debug_parallel_query;

\c postgres
DROP DATABASE regress_jsonb_tree_ops;
