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

-- A fold that meets a TOAST pointer it folded before writes the reference
-- it made then, without reading the value (the memo): an UPDATE of a small
-- column keeps the long one's pointer, and the next commit hits.  What a
-- transaction remembers is forgotten if it rolls back.  (Values in a TOAST
-- relation, as before values were stored by content; the setting puts
-- them there.)
-- (Folded by the change finder "pages", whose candidates are every row
-- on a changed page: the rows not changed are folded again, and hit.)
SET debug_toast_relation = on;
SET debug_prolly_change_finder = pages;
CREATE TABLE lv.memo (id int PRIMARY KEY, n int, body text);
ALTER TABLE lv.memo ALTER COLUMN body SET STORAGE EXTERNAL;
INSERT INTO lv.memo
SELECT g, 0, string_agg(md5(g || '/' || i), '' ORDER BY i)
FROM generate_series(1, 3) g, generate_series(1, 3000) i GROUP BY g;
SELECT count(*) FILTER (WHERE pg_column_toast_chunk_id(body) IS NOT NULL) AS out_of_line
FROM lv.memo;
SELECT count(*) AS reset FROM test_memo_counters(true);
SELECT (dolt_commit('-Am', 'memo'))[1] AS m1 \gset
SELECT hits, misses >= 3 AS each_body_read FROM test_memo_counters(true);
UPDATE lv.memo SET n = n + 1;
SELECT (dolt_commit('-am', 'memo counters'))[1] AS m2 \gset
SELECT hits, misses FROM test_memo_counters(true);
SELECT test_walk_new_count(:'m1', :'m2') <= 3 AS no_value_nodes;
SELECT md5(string_agg(id || ':' || n || ':' || body, ',' ORDER BY id)) AS memo_rows
FROM lv.memo \gset
-- (the commit's rows are the table's: read back from it)
UPDATE lv.memo SET body = 'gone';
SELECT dolt_reset('--hard', :'m2');
SELECT md5(string_agg(id || ':' || n || ':' || body, ',' ORDER BY id)) = :'memo_rows'
	AS from_history
FROM lv.memo;
-- a commit rolled back: what its fold remembered is forgotten (the
-- reset above folded the rows it wrote, and remembered their values)
INSERT INTO lv.memo
SELECT 4, 0, string_agg(md5('4/' || i), '' ORDER BY i) FROM generate_series(1, 3000) i;
BEGIN;
SELECT length(dolt_commit('-am', 'rolled back')::text) AS committed;
ROLLBACK;
SELECT count(*) AS reset FROM test_memo_counters(true);
SELECT (dolt_commit('-am', 'memo again'))[1] AS m3 \gset
SELECT hits = 3 AS others_hit, misses = 1 AS rolled_back_missed
FROM test_memo_counters(true);
SELECT md5(body) AS body4 FROM lv.memo WHERE id = 4 \gset
DELETE FROM lv.memo WHERE id = 4;
SELECT dolt_reset('--hard', :'m3');
SELECT md5(body) = :'body4' AS body4_from_history FROM lv.memo WHERE id = 4;
-- a fold in a savepoint released into one rolled back: forgotten too
INSERT INTO lv.memo
SELECT 6, 0, string_agg(md5('6/' || i), '' ORDER BY i) FROM generate_series(1, 3000) i;
BEGIN;
SAVEPOINT a;
SAVEPOINT b;
SELECT length(dolt_commit('-am', 'in a savepoint')::text) AS committed;
RELEASE b;
ROLLBACK TO a;
COMMIT;
SELECT count(*) AS reset FROM test_memo_counters(true);
SELECT length(dolt_commit('-am', 'savepoints')::text) AS committed;
SELECT hits = 4 AS others_hit, misses = 1 AS rolled_back_missed
FROM test_memo_counters(true);
DELETE FROM lv.memo WHERE id = 6;
-- a value whose first chunk is frozen isn't remembered (its xmin's age is
-- unknown): every fold reads it
INSERT INTO lv.memo
SELECT 5, 0, string_agg(md5('5/' || i), '' ORDER BY i) FROM generate_series(1, 3000) i;
VACUUM (FREEZE) lv.memo;
SELECT count(*) AS reset FROM test_memo_counters(true);
SELECT length(dolt_commit('-am', 'frozen')::text) AS committed;
UPDATE lv.memo SET n = n + 1 WHERE id = 5;
SELECT length(dolt_commit('-am', 'frozen again')::text) AS committed;
-- (the freeze made every row a candidate: the others hit in both folds)
SELECT hits = 8 AS others_hit, misses = 2 AS frozen_read_twice
FROM test_memo_counters(true);
RESET debug_prolly_change_finder;

-- A reset or merge that changes a keyed row updates it in place, and a
-- long value whose reference is the same on both sides keeps the row's
-- TOAST pointer: it isn't read, compressed or stored again.  A table with
-- another unique index is written as before (removed and inserted, which
-- a swap of unique values needs), its values stored anew.
CREATE TABLE lv.keep (id int PRIMARY KEY, n int, body text);
ALTER TABLE lv.keep ALTER COLUMN body SET STORAGE EXTERNAL;
CREATE TABLE lv.unique2 (id int PRIMARY KEY, n int UNIQUE, body text);
ALTER TABLE lv.unique2 ALTER COLUMN body SET STORAGE EXTERNAL;
INSERT INTO lv.keep
SELECT g, 0, string_agg(md5(g || '|' || i), '' ORDER BY i)
FROM generate_series(1, 2) g, generate_series(1, 2000) i GROUP BY g;
INSERT INTO lv.unique2 SELECT id, id, body FROM lv.keep;
CREATE VIEW lv.toasted AS
SELECT 'keep' AS t, id, n, pg_column_toast_chunk_id(body) AS chunk, md5(body) AS body
FROM lv.keep
UNION ALL
SELECT 'unique2', id, n, pg_column_toast_chunk_id(body), md5(body) FROM lv.unique2;
SELECT (dolt_commit('-Am', 'kept'))[1] AS p1 \gset
CREATE TEMP TABLE before_reset AS SELECT * FROM lv.toasted;
UPDATE lv.keep SET n = n + 1;
UPDATE lv.unique2 SET n = n + 10;
SELECT (dolt_commit('-am', 'kept, counted'))[1] AS p2 \gset
SELECT dolt_reset('--hard', :'p1');
SELECT t, id, a.n, a.chunk = b.chunk AS same_pointer, a.body = b.body AS same_body
FROM lv.toasted a JOIN before_reset b USING (t, id) ORDER BY t, id;

-- a body that did change is stored anew
SELECT dolt_reset('--hard', :'p2');
UPDATE lv.keep SET body = body || 'x' WHERE id = 2;
SELECT (dolt_commit('-am', 'one body changed'))[1] AS p3 \gset
DROP TABLE before_reset;
CREATE TEMP TABLE before_reset AS SELECT * FROM lv.toasted;
SELECT dolt_reset('--hard', :'p2');
SELECT t, id, a.chunk = b.chunk AS same_pointer, length(k.body)
FROM lv.toasted a JOIN before_reset b USING (t, id) JOIN lv.keep k USING (id)
WHERE t = 'keep' ORDER BY id;

-- a merge of a change to the counters keeps the pointers too
SELECT dolt_reset('--hard', :'p3');
SELECT dolt_branch('counting');
SELECT dolt_checkout('counting') IS NOT NULL AS on_counting;
UPDATE lv.keep SET n = n + 10;
SELECT length(dolt_commit('-am', 'counted on a branch')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
UPDATE lv.memo SET n = n + 1 WHERE id = 1;
SELECT length(dolt_commit('-am', 'a change on main')::text) AS committed;
DROP TABLE before_reset;
CREATE TEMP TABLE before_reset AS SELECT * FROM lv.toasted;
SELECT (dolt_merge('counting'))[2:4];
SELECT t, id, a.n, a.chunk = b.chunk AS same_pointer, a.body = b.body AS same_body
FROM lv.toasted a JOIN before_reset b USING (t, id) WHERE t = 'keep' ORDER BY id;
SELECT dolt_branch('-D', 'counting');
DROP TABLE before_reset;
RESET debug_toast_relation;

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
