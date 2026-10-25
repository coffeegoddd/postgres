-- JSON trees: a long jsonb document of a versioned table is stored as a
-- tree keyed by path, its short containers (1024 bytes or fewer) one item
-- each, and folded so.  prolly_jsonb_get() and prolly_jsonb_set() read a
-- path and set a value on the tree, prolly_jsonb_diff() diffs two trees
-- by their nodes; each gives what #>, jsonb_set() and a diff of the
-- documents give.  The tree is a function of the document: the same
-- document has the same root however it was made.
CREATE DATABASE regress_structural_jsonb ENCODING 'UTF8' LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0;
\c regress_structural_jsonb
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;

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

-- Stored as trees (form 3), all but the short one and the scalar (its
-- bytes, form 2); read back as they were, byte for byte.
SELECT id, (test_toast_pointer(j)).form AS form,
	   prolly_jsonb_tree_root(j) IS NOT NULL AS tree
FROM sj.docs ORDER BY id;
SELECT d.id, d.j = p.j AS same, d.j::text = p.j::text AS same_text,
	   prolly_canonical_value(d.j) = prolly_canonical_value(p.j) AS same_bytes
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
	(17, 1, '{items,6}', jsonb_build_object('grown', repeat('past a packed container ', 60)), true),
	(18, 1, '{items,6,grown}', '"short again"', true),
	(19, 1, '{items,10}', (SELECT jsonb_agg(g) FROM generate_series(1, 400) g), true),
	(20, 1, '{items,10,7}', '"in a long array"', true),
	(21, 1, '{items,10}', '[]', true),
	(22, 1, '{items,11,tags,-1}', '"packed: jsonb_set does it"', true),
	(23, 1, '{items,11,tags,9}', '"appended inside"', true),
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
-- a document that is a scalar: its bytes, so jsonb_set's error
SELECT prolly_jsonb_set(j, '{a}', '1', true) FROM sj.docs WHERE id = 3;
SELECT jsonb_set(j, '{a}', '1', true) FROM plain WHERE id = 3;

-- An edit writes the nodes on its path: a few, not the document's.
SELECT test_toast_counters(true) IS NOT NULL AS reset;
UPDATE sj.docs SET j = prolly_jsonb_set(j, '{key200}', '"edited"', true) WHERE id = 4;
SELECT (test_toast_counters(true)).nodes_written BETWEEN 1 AND 8 AS few_written;

-- create_if_missing is true unless said, as jsonb_set's is
SELECT prolly_jsonb_set(j, '{defaulted}', '"added"') #> '{defaulted}' AS created
FROM sj.docs WHERE id = 4;

-- A key too long for a tree's key: done whole, as jsonb_set does it (the
-- result a document in memory, which is stored as its bytes).
SELECT (test_toast_pointer(prolly_jsonb_set(j, ARRAY[repeat('k', 40000)], '1', true))).form AS form,
	   prolly_jsonb_set(j, ARRAY[repeat('k', 40000)], '1', true) =
	   jsonb_set(j, ARRAY[repeat('k', 40000)], '1', true) AS same
FROM sj.docs WHERE id = 4;

-- A document edited short enough to be inline is no tree: the edit gives
-- it whole, so it is the row a document built whole is, and a commit of
-- either has one root, without a phantom change or a merge's assertion.
CREATE TABLE sj.shrink (id int PRIMARY KEY, j jsonb);
INSERT INTO sj.shrink VALUES (1, jsonb_build_object('big',
	(SELECT left(string_agg(md5(g::text), ''), 2100) FROM generate_series(1, 70) g)));
DO $$ BEGIN FOR i IN 1..80 LOOP
	UPDATE sj.shrink SET j = prolly_jsonb_set(j, ARRAY['k' || lpad(i::text, 3, '0')], '1', true);
END LOOP; END $$;
SELECT (test_toast_pointer(j)).form AS form FROM sj.shrink;
UPDATE sj.shrink SET j = prolly_jsonb_set(j, '{big}', '""', true);
SELECT (test_toast_pointer(j)).form AS form, pg_column_size(j) = pg_column_size(j::text::jsonb) AS exact
FROM sj.shrink;
SELECT length(dolt_commit('-Am', 'shrunk')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS shrunk \gset
SELECT dolt_branch('fresh');
SELECT dolt_checkout('fresh');
UPDATE sj.shrink SET j = j::text::jsonb;
SELECT length(dolt_commit('--allow-empty', '-Am', 'fresh')::text) > 0 AS committed;
SELECT prolly_root_hash('fresh') = prolly_root_hash(:'shrunk') AS same_root;
SELECT count(*) AS changed FROM dolt_diff(:'shrunk', 'fresh', 'sj.shrink');
SELECT dolt_checkout('main');
UPDATE sj.shrink SET j = jsonb_set(j, '{k001}', '2');
SELECT length(dolt_commit('-Am', 'main k001')::text) > 0 AS committed;
SELECT length(dolt_merge('fresh')::text) > 0 AS merged;
SELECT j -> 'k001' AS k001 FROM sj.shrink;
DROP TABLE sj.shrink;
SELECT dolt_branch('-D', 'fresh');

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

-- A version from history names its tree, unread: diffing it with another
-- reads what differs, not the documents.  So does a mount of a commit.
SELECT (test_toast_pointer(j)).form AS form FROM dolt_at(:'c1', NULL::sj.docs) WHERE id = 1;
UPDATE sj.docs SET j = prolly_jsonb_set(j, '{items,12,name}', '"after c1"', true) WHERE id = 1;
SELECT test_toast_counters(true) IS NOT NULL AS reset;
SELECT * FROM prolly_jsonb_diff((SELECT j FROM dolt_at(:'c1', NULL::sj.docs) WHERE id = 1),
								(SELECT j FROM sj.docs WHERE id = 1));
SELECT (test_toast_counters(true)).nodes_read < :whole_nodes / 10 AS read_little;
SELECT mount FROM prolly_mount(:'c1', 'sj', 'sjm', NULL, false, false);
SELECT (test_toast_pointer(j)).form AS form, j = (SELECT j FROM plain WHERE id = 1) AS same
FROM sjm.docs WHERE id = 1;
SELECT prolly_unmount('sjm');
UPDATE sj.docs SET j = (SELECT j FROM dolt_at(:'c1', NULL::sj.docs) WHERE id = 1) WHERE id = 1;
SELECT count(*) AS unchanged FROM dolt_diff('HEAD', 'WORKING', 'sj.docs');

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

-- A tree may come from another server, so its packed containers' bytes
-- are checked before anything parses them: an error, not a crash.  (Leaves
-- made by hand: a packed array whose header claims a million children in
-- four bytes, one without a header, and a null to diff against.)
SELECT test_chunk_put_leaf(ARRAY['\x6a0000104001000000'::bytea]) AS bad \gset
SELECT test_chunk_put_leaf(ARRAY['\x6a'::bytea]) AS headless \gset
SELECT test_chunk_put_leaf(ARRAY['\x6e'::bytea]) AS good \gset
SELECT * FROM prolly_jsonb_diff(test_json_tree_pointer(:'good'), test_json_tree_pointer(:'bad'));
SELECT * FROM prolly_jsonb_diff(test_json_tree_pointer(:'bad'), test_json_tree_pointer(:'good'));
SELECT * FROM prolly_jsonb_diff(test_json_tree_pointer(:'good'), test_json_tree_pointer(:'headless'));
SELECT test_json_tree_pointer(:'bad')::text;

-- A whole read takes a bounded multiple of the document's jsonb size,
-- however many containers it has: here 50 paths 1000 arrays deep.
CREATE TABLE sj.deep (id int PRIMARY KEY, j jsonb);
INSERT INTO sj.deep SELECT 1, ('[' || string_agg(repeat('[', 1000) || g || repeat(']', 1000), ',')
	|| ']')::jsonb FROM generate_series(1, 50) g;
SELECT (test_toast_pointer(j)).form AS form, j = j::text::jsonb AS same FROM sj.deep;
SELECT pg_column_size(j::text::jsonb) AS doc_size FROM sj.deep \gset
SELECT jsonb_typeof(j) FROM sj.deep;
SELECT test_json_read_memory() < 20 * :doc_size AS bounded;
DROP TABLE sj.deep;

-- A document that can't be a tree (a scalar) is its bytes, as a value
-- tree; the functions read it whole, with the same results.
SELECT (test_toast_pointer(j)).form AS form, prolly_jsonb_get(j, '{}') = j AS whole_get,
	   prolly_jsonb_get(j, '{0}') AS into_scalar
FROM sj.docs WHERE id = 3;

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
SELECT dolt_branch('-D', 'built', 'side');
SELECT dolt_gc() IS NOT NULL AS collected;
SELECT d.id, d.j = p.j::jsonb AS same
FROM sj.docs d JOIN (SELECT id, j FROM dolt_at(:'c1', NULL::sj.docs)) p USING (id)
WHERE d.id IN (1, 3, 5) ORDER BY id;
SELECT count(*) AS commits_readable FROM dolt_history_table(NULL::sj.docs) WHERE (row).j IS NOT NULL;

-- A tree's strings are UTF-8 whatever the database's encoding: one cloned
-- into a LATIN1 database is read with its keys and strings converted (and
-- the keys ordered as they convert), and one stored there is built from
-- the document's UTF-8 bytes.  The functions read whole in that encoding.
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/sjson-' || txid_current() AS url \gset
CREATE TABLE sj.enc (id int PRIMARY KEY, j jsonb);
INSERT INTO sj.enc SELECT 1, jsonb_object_agg('clé' || g, 'café ' || md5(g::text))
FROM generate_series(1, 100) g;
SELECT (test_toast_pointer(j)).form AS form FROM sj.enc;
SELECT length(dolt_commit('-Am', 'encodings')::text) > 0 AS committed;
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_push('origin', 'main') AS pushed;
CREATE DATABASE regress_sj_latin1 ENCODING 'LATIN1' LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0;
\c regress_sj_latin1
SET client_encoding = 'UTF8';
SELECT dolt_clone(:'url') IS NOT NULL AS cloned;
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT j -> 'clé1' AS v, left(j::text, 30) AS text FROM sj.enc;
INSERT INTO sj.enc SELECT 2, jsonb_object_agg('ñ' || g, 'ü ' || md5(g::text))
FROM generate_series(1, 100) g;
SELECT id, (test_toast_pointer(j)).form AS form, j -> 'ñ1' AS v FROM sj.enc ORDER BY id;
SELECT prolly_jsonb_get(j, '{ñ2}') AS got, prolly_jsonb_set(j, '{ñ2}', '"é"') -> 'ñ2' AS set
FROM sj.enc WHERE id = 2;
\c regress_structural_jsonb
DROP DATABASE regress_sj_latin1;

\c postgres
DROP DATABASE regress_structural_jsonb;
