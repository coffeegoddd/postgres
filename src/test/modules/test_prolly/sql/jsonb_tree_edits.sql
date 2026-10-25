-- More of jsonb's operators on a versioned table's long documents:
-- subscripts (read and assigned), ? on arrays, ?|, ?&, #-, jsonb_insert()
-- and jsonb_set_lax() read the document's JSON tree along the path, or
-- edit the tree there, and give what they give on the document in a heap
-- table (results and errors), on random documents and random paths.
CREATE DATABASE regress_jsonb_tree_edits ENCODING 'UTF8' LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0;
\c regress_jsonb_tree_edits
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
SELECT setseed(0.986);
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


-- Subscripts: each gives what it gives on the heap table's document, by
-- text and (a first step that is one) by integer.  Rows listed are paths
-- where they differ (none).
SELECT p.id, p.path, op
FROM paths p JOIN vt.docs d USING (id) JOIN plain h USING (id),
	 LATERAL (VALUES
		('[]', CASE cardinality(p.path)
			WHEN 1 THEN d.j[p.path[1]] IS NOT DISTINCT FROM h.j[p.path[1]]
			WHEN 2 THEN d.j[p.path[1]][p.path[2]] IS NOT DISTINCT FROM h.j[p.path[1]][p.path[2]]
			WHEN 3 THEN d.j[p.path[1]][p.path[2]][p.path[3]] IS NOT DISTINCT FROM
				h.j[p.path[1]][p.path[2]][p.path[3]]
			WHEN 4 THEN d.j[p.path[1]][p.path[2]][p.path[3]][p.path[4]] IS NOT DISTINCT FROM
				h.j[p.path[1]][p.path[2]][p.path[3]][p.path[4]]
			WHEN 5 THEN d.j[p.path[1]][p.path[2]][p.path[3]][p.path[4]][p.path[5]] IS NOT DISTINCT FROM
				h.j[p.path[1]][p.path[2]][p.path[3]][p.path[4]][p.path[5]]
			WHEN 6 THEN d.j[p.path[1]][p.path[2]][p.path[3]][p.path[4]][p.path[5]][p.path[6]] IS NOT DISTINCT FROM
				h.j[p.path[1]][p.path[2]][p.path[3]][p.path[4]][p.path[5]][p.path[6]]
			ELSE true END),
		('[] int', CASE WHEN p.path[1] ~ '^-?[0-9]{1,9}$' THEN
			CASE cardinality(p.path)
				WHEN 1 THEN d.j[p.path[1]::int] IS NOT DISTINCT FROM h.j[p.path[1]::int]
				ELSE d.j[p.path[1]::int][p.path[2]] IS NOT DISTINCT FROM h.j[p.path[1]::int][p.path[2]] END
			ELSE true END),
		('?', (d.j ? p.path[1]) IS NOT DISTINCT FROM (h.j ? p.path[1]))) AS r(op, same)
WHERE NOT same
ORDER BY 1, 2, 3;

-- ?, ?| and ?&: sets of keys taken from each document's top level (an
-- object's keys, an array's string elements, early and late), with others
-- that aren't there, and NULLs.
CREATE TEMP TABLE key_sets (id int, keys text[]);
INSERT INTO key_sets
SELECT h.id, ARRAY(SELECT CASE WHEN random() < 0.1 THEN NULL
							   WHEN random() < 0.6 THEN c.k[1 + (random() * (cardinality(c.k) - 1))::int]
							   ELSE 'not ' || (random() * 10)::int END
				   FROM generate_series(1, n.n))
FROM plain h,
	 LATERAL (SELECT ARRAY(SELECT k FROM jsonb_object_keys(CASE WHEN jsonb_typeof(h.j) = 'object' THEN h.j ELSE '{}' END) k
						   UNION ALL
						   SELECT e FROM jsonb_array_elements_text(CASE WHEN jsonb_typeof(h.j) = 'array' THEN h.j ELSE '[]' END) e
						   UNION ALL SELECT 'k1') AS k) c,
	 generate_series(1, 60) g,
	 LATERAL (SELECT g % 5 AS n) n;
SELECT count(*) > 500 AS many_sets FROM key_sets;
SELECT s.id, s.keys, op
FROM key_sets s JOIN vt.docs d USING (id) JOIN plain h USING (id),
	 LATERAL (VALUES
		('?|', (d.j ?| s.keys) = (h.j ?| s.keys)),
		('?&', (d.j ?& s.keys) = (h.j ?& s.keys)),
		('?', (d.j ? s.keys[1]) IS NOT DISTINCT FROM (h.j ? s.keys[1]))) AS r(op, same)
WHERE NOT same
ORDER BY 1, 2::text, 3;
SELECT count(*) FILTER (WHERE h.j ?| s.keys) > 0 AS some_any,
	   count(*) FILTER (WHERE NOT h.j ?| s.keys) > 0 AS some_none,
	   count(*) FILTER (WHERE h.j ?& s.keys AND cardinality(s.keys) > 1) > 0 AS some_all,
	   count(*) FILTER (WHERE jsonb_typeof(h.j) = 'array' AND h.j ? s.keys[1]) > 0 AS array_strings
FROM key_sets s JOIN plain h USING (id);

-- Edits: #-, jsonb_insert() before and after, jsonb_set_lax() with a value
-- and with NULL in each of its ways, on the paths with values of every
-- kind: the documents they give, or (shown below) their errors.
CREATE TEMP TABLE edit_values (n int, v jsonb);
INSERT INTO edit_values VALUES (0, '"put"'), (1, 'null'), (2, '{"n": [1, 2.50]}'), (3, '[]'),
	(4, to_jsonb(repeat('grown ', 300))), (5, '-0.000');
CREATE FUNCTION do_edit(op text, j jsonb, path text[], v jsonb) RETURNS jsonb
LANGUAGE sql AS $$
	SELECT CASE op
		WHEN '#-' THEN j #- path
		WHEN 'insert' THEN jsonb_insert(j, path, v)
		WHEN 'insert after' THEN jsonb_insert(j, path, v, true)
		WHEN 'lax' THEN jsonb_set_lax(j, path, v)
		WHEN 'lax delete_key' THEN jsonb_set_lax(j, path, NULL, true, 'delete_key')
		WHEN 'lax return_target' THEN jsonb_set_lax(j, path, NULL, true, 'return_target')
		WHEN 'lax use_json_null' THEN jsonb_set_lax(j, path, NULL, false, 'use_json_null')
		WHEN 'lax raise_exception' THEN jsonb_set_lax(j, path, NULL, true, 'raise_exception') END
$$;
CREATE FUNCTION try_edit(op text, j jsonb, path text[], v jsonb) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
	RETURN do_edit(op, j, path, v)::text;
EXCEPTION WHEN OTHERS THEN
	RETURN 'error: ' || SQLERRM;
END $$;
CREATE TEMP TABLE edit_cases AS
SELECT p.id, p.path, (random() * 5)::int AS n,
	   (ARRAY['#-', 'insert', 'insert after', 'lax', 'lax delete_key', 'lax return_target',
			  'lax use_json_null', 'lax raise_exception'])[1 + (random() * 7)::int] AS op
FROM paths p WHERE random() < 0.4;
-- (the heap's document tells which are errors: try_edit's subtransaction
-- takes no tree edit, so the others are compared outside one)
ALTER TABLE edit_cases ADD COLUMN err text;
UPDATE edit_cases e SET err = (SELECT CASE WHEN r LIKE 'error:%' THEN r END
							   FROM plain h, edit_values v, try_edit(e.op, h.j, e.path, v.v) r
							   WHERE h.id = e.id AND v.n = e.n);
SELECT count(*) > 1500 AS many_edits, count(DISTINCT op) AS ops,
	   count(*) FILTER (WHERE err IS NOT NULL) > 0 AS errors
FROM edit_cases;
WITH ok AS MATERIALIZED (SELECT * FROM edit_cases WHERE err IS NULL)
SELECT e.id, e.path, e.n, e.op
FROM ok e JOIN edit_values USING (n) JOIN vt.docs d USING (id) JOIN plain h USING (id)
WHERE do_edit(e.op, d.j, e.path, v)::text IS DISTINCT FROM do_edit(e.op, h.j, e.path, v)::text
ORDER BY 1, 2, 3, 4;
-- the errors, each statement on its own (a tree edit would give a row),
-- after the heap's error
SELECT format('SELECT %L AS heap; SELECT do_edit(%L, j, %L, %L) FROM vt.docs WHERE id = %s',
			  e.err, e.op, e.path, v.v, e.id)
FROM (SELECT DISTINCT ON (op, err) * FROM edit_cases WHERE err IS NOT NULL AND n <> 4
	  ORDER BY op, err, id, path::text, n) e
	 JOIN edit_values v USING (n)
ORDER BY e.op, e.err, e.id \gexec

-- A sequence of edits by UPDATE, of each kind (a subscript's assignment
-- among them): each step gives what the heap table's does, and the tree's
-- root is the root of the document built whole.  A step the heap's
-- document refuses is left out.
CREATE TEMP TABLE edit_results (step int, op text, same bool, same_root bool);
DO $$
DECLARE
	e record;
	r1 text;
	r2 text;
	k int := 0;
	subs text;
	upd text;
BEGIN
	FOR e IN SELECT s.id, s.path, v.v,
					(ARRAY['[]=', '#-', 'insert', 'insert after', 'lax delete_key'])[1 + s.k % 5] AS op
			 FROM (SELECT p.*, row_number() OVER () AS k
				   FROM (SELECT * FROM paths
						 WHERE path IS NOT NULL AND array_position(path, NULL) IS NULL
							   AND cardinality(path) > 0
						 ORDER BY random() LIMIT 250) p) s
				  JOIN edit_values v ON v.n = s.k % 6
			 ORDER BY s.k LOOP
		SELECT string_agg(format('[%L]', s), '') INTO subs FROM unnest(e.path) s;
		upd := CASE e.op
			WHEN '[]=' THEN format('UPDATE %%s SET j%s = %L WHERE id = %s', subs, e.v, e.id)
			ELSE format('UPDATE %%s SET j = do_edit(%L, j, %L, %L) WHERE id = %s', e.op, e.path, e.v, e.id) END;
		BEGIN
			EXECUTE format(upd, 'plain');
		EXCEPTION WHEN OTHERS THEN
			CONTINUE;
		END;
		k := k + 1;
		EXECUTE format(upd, 'vt.docs');
		DELETE FROM vt.docs WHERE id = 100;
		INSERT INTO vt.docs SELECT 100, j FROM plain WHERE id = e.id;
		SELECT prolly_jsonb_tree_root(j) INTO r1 FROM vt.docs WHERE id = e.id;
		SELECT prolly_jsonb_tree_root(j) INTO r2 FROM vt.docs WHERE id = 100;
		INSERT INTO edit_results
		SELECT k, e.op, d.j = h.j, r1 IS NOT DISTINCT FROM r2
		FROM vt.docs d JOIN plain h USING (id) WHERE id = e.id;
	END LOOP;
	DELETE FROM vt.docs WHERE id = 100;
END $$;
SELECT op, count(*) > 10 AS steps, count(*) FILTER (WHERE NOT (same AND same_root)) AS differ
FROM edit_results GROUP BY op ORDER BY op;
-- (the documents, all edited, as the heap's)
SELECT count(*) FILTER (WHERE d.j = h.j) AS same, count(*) AS docs
FROM vt.docs d JOIN plain h USING (id);

-- Each reads the nodes on its path, not the document's: fewer than a
-- tenth of the nodes a whole read reads, and so does each edit by UPDATE.
CREATE TABLE vt.big (id int PRIMARY KEY, j jsonb);
INSERT INTO vt.big
SELECT 1, jsonb_build_object('items', jsonb_object_agg('k' || g, jsonb_build_object('id', g, 'name', md5(g::text))),
							 'meta', '{"n": 1}'::jsonb, 'arr', (SELECT jsonb_agg(md5(h::text)) FROM generate_series(1, 2000) h))
FROM generate_series(1, 20000) g;
INSERT INTO vt.big SELECT 2, jsonb_agg(md5(g::text)) FROM generate_series(1, 20000) g;
CREATE TABLE big_plain AS SELECT * FROM vt.big;
SELECT test_toast_counters(true) IS NOT NULL AS reset;
SELECT length(j::text) > 0 AS read_whole FROM vt.big WHERE id = 1;
SELECT (test_toast_counters(true)).nodes_read AS whole_nodes \gset
CREATE TEMP TABLE reads (op text, nodes bigint);
SELECT j['items']['k77']['name'] FROM vt.big WHERE id = 1 \g /dev/null
INSERT INTO reads SELECT '[]', (test_toast_counters(true)).nodes_read;
SELECT j['arr'][-3] FROM vt.big WHERE id = 1 \g /dev/null
INSERT INTO reads SELECT '[] negative', (test_toast_counters(true)).nodes_read;
SELECT j ?| '{nope,meta}' FROM vt.big WHERE id = 1 \g /dev/null
INSERT INTO reads SELECT '?|', (test_toast_counters(true)).nodes_read;
SELECT j ?& '{items,meta,arr}' FROM vt.big WHERE id = 1 \g /dev/null
INSERT INTO reads SELECT '?&', (test_toast_counters(true)).nodes_read;
SELECT j ? 'items' FROM vt.big WHERE id = 1 \g /dev/null
INSERT INTO reads SELECT '? (a long value)', (test_toast_counters(true)).nodes_read;
UPDATE vt.big SET j['items']['k77']['name'] = '"assigned"' WHERE id = 1;
INSERT INTO reads SELECT '[]=', (test_toast_counters(true)).nodes_read;
UPDATE vt.big SET j = j #- '{items,k78}' WHERE id = 1;
INSERT INTO reads SELECT '#-', (test_toast_counters(true)).nodes_read;
UPDATE vt.big SET j = jsonb_insert(j, '{items,k20001}', '{"id": 20001}') WHERE id = 1;
INSERT INTO reads SELECT 'jsonb_insert', (test_toast_counters(true)).nodes_read;
UPDATE vt.big SET j = jsonb_insert(j, '{arr,-1}', '"appended"', true) WHERE id = 1;
INSERT INTO reads SELECT 'jsonb_insert appended', (test_toast_counters(true)).nodes_read;
UPDATE vt.big SET j = jsonb_set_lax(j, '{items,k79}', NULL, true, 'delete_key') WHERE id = 1;
INSERT INTO reads SELECT 'jsonb_set_lax delete_key', (test_toast_counters(true)).nodes_read;
UPDATE vt.big SET j = jsonb_set_lax(j, '{items,k80,name}', '"lax"') WHERE id = 1;
INSERT INTO reads SELECT 'jsonb_set_lax', (test_toast_counters(true)).nodes_read;
UPDATE vt.big SET j = jsonb_set_lax(j, '{items,k81}', NULL, true, 'return_target') WHERE id = 1;
INSERT INTO reads SELECT 'jsonb_set_lax return_target', (test_toast_counters(true)).nodes_read;
SELECT op, nodes > 0 AND nodes * 10 < :whole_nodes AS path_reads_less FROM reads ORDER BY op;
-- (and on the array: an early string found, the last element removed)
SELECT length(j::text) > 0 AS read_whole FROM vt.big WHERE id = 2;
SELECT (test_toast_counters(true)).nodes_read AS whole_array_nodes \gset
SELECT j ? md5('3') FROM vt.big WHERE id = 2 \g /dev/null
SELECT (test_toast_counters(true)).nodes_read AS exists_nodes \gset
UPDATE vt.big SET j = j #- '{-1}' WHERE id = 2;
SELECT (test_toast_counters(true)).nodes_read AS pop_nodes \gset
SELECT :exists_nodes > 0 AND :exists_nodes * 10 < :whole_array_nodes AS exists_reads_less,
	   :pop_nodes > 0 AND :pop_nodes * 10 < :whole_array_nodes AS pop_reads_less;
-- the same edits on the heap table: the documents are the same, trees
UPDATE big_plain SET j['items']['k77']['name'] = '"assigned"' WHERE id = 1;
UPDATE big_plain SET j = j #- '{items,k78}' WHERE id = 1;
UPDATE big_plain SET j = jsonb_insert(j, '{items,k20001}', '{"id": 20001}') WHERE id = 1;
UPDATE big_plain SET j = jsonb_insert(j, '{arr,-1}', '"appended"', true) WHERE id = 1;
UPDATE big_plain SET j = jsonb_set_lax(j, '{items,k79}', NULL, true, 'delete_key') WHERE id = 1;
UPDATE big_plain SET j = jsonb_set_lax(j, '{items,k80,name}', '"lax"') WHERE id = 1;
UPDATE big_plain SET j = j #- '{-1}' WHERE id = 2;
SELECT d.id, d.j = h.j AS same, (test_toast_pointer(d.j)).form AS form
FROM vt.big d JOIN big_plain h USING (id) ORDER BY 1;

-- An edit that empties a long container, or the document: the container
-- packed, the document no longer a tree.
CREATE TABLE vt.one (id int PRIMARY KEY, j jsonb);
INSERT INTO vt.one VALUES (1, jsonb_build_object('only', jsonb_build_object('inner', repeat('x', 3000)), 'k', 1)),
	(2, jsonb_build_object('only', repeat('y', 3000)));
CREATE TABLE one_plain AS SELECT * FROM vt.one;
UPDATE vt.one SET j = j #- '{only,inner}' WHERE id = 1;
UPDATE one_plain SET j = j #- '{only,inner}' WHERE id = 1;
UPDATE vt.one SET j = j #- '{only}' WHERE id = 2;
UPDATE one_plain SET j = j #- '{only}' WHERE id = 2;
SELECT d.id, d.j, d.j = h.j AS same, (test_toast_pointer(d.j)).form AS form
FROM vt.one d JOIN one_plain h USING (id) ORDER BY 1;

-- No tree edit where its nodes wouldn't outlive the result, or can't be
-- written: the edits take the document whole, giving what they give.
BEGIN READ ONLY;
SELECT (j #- '{items,k82}') ? 'items' AS deleted,
	   test_toast_pointer(j #- '{items,k82}') IS NULL AS whole,
	   test_toast_pointer(jsonb_insert(j, '{items,new}', '1')) IS NULL AS insert_whole
FROM vt.big WHERE id = 1;
SELECT pg_current_xact_id_if_assigned() IS NULL AS no_xid;
COMMIT;
CREATE FUNCTION edit_in_subxact() RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	x jsonb;
BEGIN
	BEGIN
		SELECT j #- '{items,k5}' INTO x FROM vt.big WHERE id = 1;
		RAISE EXCEPTION 'boom';
	EXCEPTION WHEN OTHERS THEN
		NULL;
	END;
	INSERT INTO vt.big VALUES (4, x);
	RETURN (x -> 'items' ? 'k5')::text;
END $$;
SELECT edit_in_subxact();
SELECT (SELECT j FROM vt.big WHERE id = 4) = ((SELECT j FROM vt.big WHERE id = 1) #- '{items,k5}') AS same;

\c postgres
DROP DATABASE regress_jsonb_tree_edits;
