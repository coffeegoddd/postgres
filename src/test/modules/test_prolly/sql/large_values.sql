-- Large values in versioned tables: TOAST holds them in the working set;
-- a commit stores them out of line, in value trees addressed by byte
-- counts.  They read back whole from history, and a few bytes inserted
-- in the middle of one add a few nodes to the store, not one per chunk
-- after them.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA lv VERSIONED;
CREATE TABLE lv.big (id int PRIMARY KEY, t text, b bytea);
INSERT INTO lv.big
SELECT g, string_agg(md5(g || ':' || i), '' ORDER BY i),
	decode(repeat(to_hex(g * 17), 30000), 'hex')
FROM generate_series(1, 5) g, generate_series(1, 2000 * g) i GROUP BY g;
CREATE TABLE lv.keyless (t text);
INSERT INTO lv.keyless SELECT repeat('k', 100000) FROM generate_series(1, 3);
CREATE VIEW lv.digest AS
SELECT string_agg(id || ':' || length(t) || ':' || md5(t) || ':' || md5(b), ','
				  ORDER BY id) AS d
FROM lv.big;
SELECT d AS before FROM lv.digest \gset
SELECT left(:'before', 9) AS first_row;
SELECT (dolt_commit('-Am', 'large values'))[1] AS c1 \gset

-- Three bytes inserted into the middle of a 192 KB value.
UPDATE lv.big SET t = overlay(t PLACING 'XYZ' FROM 96000 FOR 0) WHERE id = 3;
SELECT length(t) FROM lv.big WHERE id = 3;
SELECT d AS after FROM lv.digest \gset
SELECT (dolt_commit('-am', 'an insertion'))[1] AS c2 \gset
SELECT test_walk_new_count(:'c1', :'c2') < 20 AS few_nodes_added;
SELECT diff_type, (from_row).id, length((from_row).t), length((to_row).t)
FROM dolt_diff(:'c1', :'c2', NULL::lv.big);

-- Back to the first commit and forward again, from history alone.
SELECT dolt_reset('--hard', :'c1');
SELECT d = :'before' AS restored FROM lv.digest;
DELETE FROM lv.keyless;
SELECT dolt_reset('--hard', :'c2');
SELECT d = :'after' AS forward FROM lv.digest;
SELECT count(*), sum(length(t)) FROM lv.keyless;

-- A row's long values are each out of line in a value tree of their own,
-- named by their bytes alone: a small column's update leaves its row's
-- long value's tree as it was, and a value copied into another row (or
-- another table) adds nothing to the store but the row.
CREATE TABLE lv.doc (id int PRIMARY KEY, n int, body text);
INSERT INTO lv.doc
SELECT 1, 0, string_agg(md5('doc' || i), '' ORDER BY i)
FROM generate_series(1, 30000) i;
SELECT (dolt_commit('-Am', 'a 960 KB document'))[1] AS d1 \gset
UPDATE lv.doc SET n = n + 1;
SELECT (dolt_commit('-am', 'its counter'))[1] AS d2 \gset
-- (the commit, its root and the table's leaf: no node of the value's tree)
SELECT test_walk_new_count(:'d1', :'d2') <= 3 AS small_update_few_nodes;
INSERT INTO lv.doc SELECT 2, 0, body FROM lv.doc WHERE id = 1;
CREATE TABLE lv.copy (body text);
INSERT INTO lv.copy SELECT body FROM lv.doc WHERE id = 1;
SELECT (dolt_commit('-Am', 'copies'))[1] AS d3 \gset
-- (the commit, its root and a leaf of each table)
SELECT test_walk_new_count(:'d2', :'d3') <= 4 AS copies_few_nodes;
SELECT diff_type, (to_row).id, (to_row).n, length((to_row).body)
FROM dolt_diff(:'d1', :'d3', NULL::lv.doc) ORDER BY 2;

-- A long primary key: the key encodes the whole value; the row holds a
-- reference.  Rows keyed by it are found again across folds, and come
-- back from history.
CREATE TABLE lv.longkey (k text PRIMARY KEY, v int);
INSERT INTO lv.longkey
SELECT repeat(chr(97 + g), 3000) || g, g FROM generate_series(1, 5) g;
SELECT (dolt_commit('-Am', 'long keys'))[1] AS k1 \gset
UPDATE lv.longkey SET v = v * 10 WHERE v % 2 = 0;
DELETE FROM lv.longkey WHERE v = 5;
SELECT (dolt_commit('-am', 'long keys changed'))[1] AS k2 \gset
SELECT diff_type, length(coalesce((from_row).k, (to_row).k)), (from_row).v, (to_row).v
FROM dolt_diff(:'k1', :'k2', NULL::lv.longkey) ORDER BY 3;
SELECT dolt_reset('--hard', :'k1');
SELECT length(k), v FROM lv.longkey ORDER BY v;
SELECT dolt_reset('--hard', :'k2');
SELECT length(k), v FROM lv.longkey ORDER BY v;

-- A full collection of the chunk store keeps every value tree the
-- commits' rows reference (each leaf names them, as walks find them), so
-- history still reads back whole.
SELECT md5(string_agg(id || ':' || n || ':' || md5(body), ',' ORDER BY id)) AS docs_gc
FROM lv.doc \gset
SELECT kept > 0 AS kept FROM test_chunk_gc(true);
DELETE FROM lv.doc;
UPDATE lv.longkey SET v = 0;
SELECT dolt_reset('--hard', :'d3');
SELECT md5(string_agg(id || ':' || n || ':' || md5(body), ',' ORDER BY id)) = :'docs_gc'
	AS docs_after_gc
FROM lv.doc;
SELECT dolt_reset('--hard', :'k1');
SELECT length(k), v FROM lv.longkey ORDER BY v;
SELECT dolt_reset('--hard', :'k2');

-- Pushed and cloned: a walk from a commit reaches every value tree its
-- rows reference, so the clone reads them all.
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/large-' || txid_current() AS url \gset
SELECT current_database() AS db \gset
SELECT d AS digest FROM lv.digest \gset
SELECT md5(string_agg(id || ':' || n || ':' || body, ',' ORDER BY id)) AS docs FROM lv.doc \gset
SELECT dolt_remote('add', 'large', :'url');
SELECT dolt_push('large', 'main') AS pushed;
CREATE DATABASE regress_large_clone;
\c regress_large_clone
SELECT dolt_clone(:'url');
SELECT d = :'digest' AS same_values FROM lv.digest;
SELECT md5(string_agg(id || ':' || n || ':' || body, ',' ORDER BY id)) = :'docs' AS same_docs
FROM lv.doc;
SELECT count(*), sum(length(t)) FROM lv.keyless;
\c :db
DROP DATABASE regress_large_clone;
SELECT dolt_remote('remove', 'large');

DROP SCHEMA lv CASCADE;
SELECT length(dolt_commit('-Am', 'lv gone')::text) AS committed;
