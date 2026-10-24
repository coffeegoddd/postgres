-- JSON trees: with prolly.structural_jsonb on, a long jsonb document of a
-- versioned table is stored as a tree of its scalars keyed by path, and
-- folded so.  prolly_jsonb_get() and prolly_jsonb_set() read a path and
-- set a value on the tree, prolly_jsonb_diff() diffs two trees by their
-- nodes; each gives what #>, jsonb_set() and a diff of the documents give.
-- The tree is a function of the document: the same document has the same
-- root however it was made.
CREATE DATABASE regress_structural_jsonb ENCODING 'UTF8' LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0;
\c regress_structural_jsonb
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
SET prolly.structural_jsonb = on;

CREATE SCHEMA sj VERSIONED;
CREATE TABLE sj.docs (id int PRIMARY KEY, j jsonb);
-- the same documents in a heap table, as PostgreSQL has them
CREATE TABLE plain (id int PRIMARY KEY, j jsonb);

-- Nested objects and arrays, empty containers, JSON null, numerics as
-- entered (1.50 keeps its scale), long strings (out of line in the tree),
-- non-ASCII keys and strings, keys of one length and of several (jsonb
-- sorts shorter first), more than an offset stride (32) of children, and
-- documents that are an array or a scalar.
INSERT INTO plain
SELECT 1, jsonb_build_object(
		'items', jsonb_agg(jsonb_build_object('id', g, 'name', md5(g::text),
											  'price', g * 1.50, 'tags', jsonb_build_array(g, 'x', null),
											  'none', null::jsonb, 'empty', '{}'::jsonb,
											  'list', '[]'::jsonb) ORDER BY g),
		'meta', jsonb_build_object('é', 'ü€', 'b', true, 'aa', false, 'c', -0.000,
								   'big', repeat('long string ', 400)),
		'z', '{}'::jsonb, 'nested', '[[[{"deep": [[]]}]]]'::jsonb)
FROM generate_series(1, 2000) g;
INSERT INTO plain
SELECT 2, jsonb_agg(jsonb_build_array(g, md5(g::text), jsonb_build_object('k' || g, g)) ORDER BY g)
FROM generate_series(1, 200) g;
INSERT INTO plain SELECT 3, to_jsonb(string_agg(md5(g::text), ' ')) FROM generate_series(1, 300) g;
INSERT INTO plain SELECT 4, jsonb_object_agg('key' || g, g) FROM generate_series(1, 400) g;
-- short: inline, not a tree
INSERT INTO plain VALUES (5, '{"small": [1, 2, 3]}');
INSERT INTO sj.docs SELECT * FROM plain;

-- Stored as trees (form 3), all but the short one; read back as they were.
SELECT id, (test_toast_pointer(j)).form AS form,
	   prolly_jsonb_tree_root(j) IS NOT NULL AS tree
FROM sj.docs ORDER BY id;
SELECT d.id, d.j = p.j AS same, d.j::text = p.j::text AS same_text
FROM sj.docs d JOIN plain p USING (id) ORDER BY id;

-- Paths read from the tree give what #> gives: objects, arrays, scalars,
-- empty containers, JSON null (not SQL NULL), nothing there, a path into a
-- scalar, an index that isn't one, a negative index (read whole), the
-- whole document.
CREATE TEMP TABLE paths (path text[]);
INSERT INTO paths VALUES ('{items,5}'), ('{items,5,name}'), ('{items,5,price}'),
	('{items,5,tags}'), ('{items,5,tags,2}'), ('{items,5,none}'), ('{items,5,empty}'),
	('{items,5,list}'), ('{items,5,empty,x}'), ('{items,1999,id}'), ('{items,2000}'),
	('{items,x}'), ('{items,-1,id}'), ('{items,+3,id}'), ('{meta}'), ('{meta,é}'),
	('{meta,big}'), ('{meta,c}'), ('{nope}'), ('{meta,b,x}'), ('{nested,0,0,0,deep}'),
	('{z}'), ('{0}'), ('{0,1}'), ('{199,2,k200}'), ('{key400}'), ('{}'),
	('{items,NULL}');
SELECT d.id, count(*) AS paths,
	   count(*) FILTER (WHERE prolly_jsonb_get(d.j, path) IS NOT DISTINCT FROM p.j #> path) AS as_hash_gt,
	   count(*) FILTER (WHERE p.j #> path IS NOT NULL) AS found
FROM sj.docs d JOIN plain p USING (id), paths GROUP BY d.id ORDER BY d.id;
SELECT prolly_jsonb_get(j, '{items,5,none}') AS json_null,
	   prolly_jsonb_get(j, '{items,5,none}') IS NULL AS sql_null,
	   prolly_jsonb_get(j, '{items,5,price}') AS scale_kept
FROM sj.docs WHERE id = 1;

-- A path read reads the nodes on its path, not the document's: fewer than
-- a tenth of the nodes a whole read reads.
SELECT test_toast_counters(true) IS NOT NULL AS reset;
SELECT prolly_jsonb_get(j, '{items,50,name}') FROM sj.docs WHERE id = 1;
SELECT (test_toast_counters(true)).nodes_read AS path_nodes \gset
SELECT length(j::text) > 0 AS read_whole FROM sj.docs WHERE id = 1;
SELECT (test_toast_counters(true)).nodes_read AS whole_nodes \gset
SELECT :path_nodes > 0 AND :path_nodes * 10 < :whole_nodes AS path_reads_less;

-- Edits on the tree give what jsonb_set gives, each step of a sequence:
-- a value replaced, a key added (into an empty object too), a container
-- set to a scalar and back, an array element replaced, a value set to
-- what it was, a missing key without create, a path through a scalar,
-- and what the tree leaves to a whole edit (an index past the end, a
-- negative one, a scalar document).  After each, the tree's root is the
-- root a document built whole has.
CREATE TEMP TABLE edits (n int, id int, path text[], v jsonb, c bool);
INSERT INTO edits VALUES
	(1, 1, '{items,5,name}', '"renamed"', true),
	(2, 1, '{items,5,added}', '{"new": [1, 2]}', true),
	(3, 1, '{z,first}', '1', true),
	(4, 1, '{items,7}', '"now a scalar"', true),
	(5, 1, '{items,7}', '{"back": {"to": "an object"}}', true),
	(6, 1, '{items,8,tags,1}', 'null', true),
	(7, 1, '{items,9,price}', '1.50', true),
	(8, 1, '{items,9,missing}', '1', false),
	(9, 1, '{meta,b,x}', '1', true),
	(10, 1, '{items,2000}', '"appended"', true),
	(11, 1, '{items,-1}', '"last"', true),
	(12, 1, '{meta,big}', to_jsonb(repeat('another long string ', 300)), true),
	(13, 2, '{150,2,k151}', '[]', true),
	(15, 4, '{key0}', '0', true),
	(16, 4, '{key400}', '"four hundred"', true);
CREATE TEMP TABLE edit_results (n int, same bool, same_root bool);
DO $$
DECLARE
	e record;
	r1 text;
	r2 text;
BEGIN
	FOR e IN SELECT * FROM edits ORDER BY n LOOP
		UPDATE sj.docs SET j = prolly_jsonb_set(j, e.path, e.v, e.c) WHERE id = e.id;
		UPDATE plain SET j = jsonb_set(j, e.path, e.v, e.c) WHERE id = e.id;
		DELETE FROM sj.docs WHERE id = 100;
		INSERT INTO sj.docs SELECT 100, j FROM plain WHERE id = e.id;
		SELECT prolly_jsonb_tree_root(j) INTO r1 FROM sj.docs WHERE id = e.id;
		SELECT prolly_jsonb_tree_root(j) INTO r2 FROM sj.docs WHERE id = 100;
		INSERT INTO edit_results
		SELECT e.n, d.j = p.j, r1 IS NOT DISTINCT FROM r2
		FROM sj.docs d JOIN plain p USING (id) WHERE id = e.id;
	END LOOP;
	DELETE FROM sj.docs WHERE id = 100;
END $$;
SELECT * FROM edit_results ORDER BY n;
-- a document that is a scalar: jsonb_set's error
SELECT prolly_jsonb_set(j, '{a}', '1', true) FROM sj.docs WHERE id = 3;
SELECT jsonb_set(j, '{a}', '1', true) FROM plain WHERE id = 3;

-- An edit writes the nodes on its path: a few, not the document's.
SELECT test_toast_counters(true) IS NOT NULL AS reset;
UPDATE sj.docs SET j = prolly_jsonb_set(j, '{key200}', '"edited"', true) WHERE id = 4;
SELECT (test_toast_counters(true)).nodes_written BETWEEN 1 AND 8 AS few_written;

-- Diffs of trees give what a diff of the documents gives.
CREATE TEMP TABLE before_docs AS SELECT * FROM sj.docs;
UPDATE sj.docs SET j = prolly_jsonb_set(prolly_jsonb_set(j, '{items,3,name}', '"diffed"', true),
										'{items,4}', '{"replaced": true}', true)
WHERE id = 1;
SELECT * FROM prolly_jsonb_diff((SELECT j FROM before_docs WHERE id = 1),
								(SELECT j FROM sj.docs WHERE id = 1));
SELECT * FROM prolly_jsonb_diff((SELECT j::text::jsonb FROM before_docs WHERE id = 1),
								(SELECT j::text::jsonb FROM sj.docs WHERE id = 1));
SELECT count(*) AS no_change FROM prolly_jsonb_diff((SELECT j FROM sj.docs WHERE id = 2),
													(SELECT j FROM sj.docs WHERE id = 2));
UPDATE plain p SET j = d.j::text::jsonb FROM sj.docs d WHERE d.id = p.id;

-- A commit folds the trees unread, and history gives the documents back.
SELECT length(dolt_commit('-Am', 'trees')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS c1 \gset
SELECT d.id, d.j = p.j AS same
FROM dolt_at(:'c1', NULL::sj.docs) d JOIN plain p USING (id) ORDER BY id;
SELECT prolly_jsonb_get(j, '{items,3,name}') FROM dolt_at(:'c1', NULL::sj.docs) WHERE id = 1;

-- The same documents reached another way (each built whole on a branch)
-- give the same root.
SELECT dolt_branch('built');
SELECT dolt_checkout('built');
DELETE FROM sj.docs;
INSERT INTO sj.docs SELECT id, j::text::jsonb FROM plain;
-- (nothing to commit: the working set's trees are the commit's)
SELECT length(dolt_commit('--allow-empty', '-Am', 'built whole')::text) > 0 AS committed;
SELECT prolly_root_hash('built') = prolly_root_hash(:'c1') AS same_root;
SELECT dolt_checkout('main');

-- Folded with the setting off, the documents are their bytes again: the
-- same documents, another root.
SELECT dolt_branch('bytes');
SELECT dolt_checkout('bytes');
SET prolly.structural_jsonb = off;
DELETE FROM sj.docs;
INSERT INTO sj.docs SELECT id, j::text::jsonb FROM plain;
SELECT (test_toast_pointer(j)).form AS form FROM sj.docs WHERE id = 1;
SELECT length(dolt_commit('-Am', 'bytes')::text) > 0 AS committed;
SELECT prolly_root_hash('bytes') <> prolly_root_hash(:'c1') AS another_root;
SELECT d.id, d.j = p.j AS same
FROM dolt_at('bytes', NULL::sj.docs) d JOIN plain p USING (id) ORDER BY id;
-- the functions read such documents whole, with the same results
SELECT prolly_jsonb_get(j, '{items,3,name}') AS whole_get,
	   prolly_jsonb_set(j, '{items,3,name}', '"x"', true) #> '{items,3,name}' AS whole_set
FROM sj.docs WHERE id = 1;
SET prolly.structural_jsonb = on;
SELECT dolt_checkout('main');

-- A merge writes rows from history: a document changed on a branch comes
-- back as its tree.
SELECT dolt_branch('side');
SELECT dolt_checkout('side');
UPDATE sj.docs SET j = prolly_jsonb_set(j, '{key1}', '"from side"', true) WHERE id = 4;
SELECT length(dolt_commit('-Am', 'side edit')::text) > 0 AS committed;
SELECT dolt_checkout('main');
UPDATE sj.docs SET j = prolly_jsonb_set(j, '{0,0}', '"from main"', true) WHERE id = 2;
SELECT length(dolt_commit('-Am', 'main edit')::text) > 0 AS committed;
SELECT length(dolt_merge('side')::text) > 0 AS merged;
SELECT id, (test_toast_pointer(j)).form AS form, prolly_jsonb_get(j, '{key1}') AS key1,
	   prolly_jsonb_get(j, '{0,0}') AS first
FROM sj.docs WHERE id IN (2, 4) ORDER BY id;

-- A collection keeps every tree the pages and the commits name.
SELECT dolt_branch('-D', 'built', 'bytes', 'side');
SELECT dolt_gc() IS NOT NULL AS collected;
SELECT d.id, d.j = p.j::jsonb AS same
FROM sj.docs d JOIN (SELECT id, j FROM dolt_at(:'c1', NULL::sj.docs)) p USING (id)
WHERE d.id IN (1, 3, 5) ORDER BY id;
SELECT count(*) AS commits_readable FROM dolt_history_table(NULL::sj.docs) WHERE (row).j IS NOT NULL;

\c postgres
DROP DATABASE regress_structural_jsonb;
