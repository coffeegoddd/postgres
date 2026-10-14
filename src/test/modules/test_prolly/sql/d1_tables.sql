--
-- Tables stored by engine D1: rows are tree items, written privately by
-- each transaction and published at commit.
--
\set VERBOSITY terse
SET default_table_access_method = prolly_d1;
-- Fetching rows by TID (index scans) comes later.
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_indexonlyscan = off;

-- A table without a primary key is keyed by content: identical rows are
-- one item with a count, but each row has its own TID.
CREATE TABLE d1_keyless (a int, b text);
INSERT INTO d1_keyless VALUES (1, 'x'), (2, 'y'), (1, 'x'), (NULL, NULL);
SELECT a, b, count(*), count(DISTINCT ctid) AS tids
FROM d1_keyless GROUP BY a, b ORDER BY a NULLS LAST;

-- Rows inserted in one command don't see each other, and the statement
-- doesn't see its own rows (Halloween protection).
INSERT INTO d1_keyless SELECT * FROM d1_keyless;
SELECT count(*), count(DISTINCT ctid) FROM d1_keyless;

-- A table with a primary key, filled in bulk and in one transaction.
CREATE TABLE d1_pk (id int PRIMARY KEY, v text);
INSERT INTO d1_pk SELECT g, 'v' || g FROM generate_series(1, 5000) g;
SELECT count(*), sum(id), min(v), max(v) FROM d1_pk;

-- Savepoints: a rolled-back subtransaction's rows disappear, and earlier
-- ones stay.
BEGIN;
INSERT INTO d1_pk VALUES (10001, 'kept');
SAVEPOINT s1;
INSERT INTO d1_pk SELECT g, 'gone' FROM generate_series(20001, 21000) g;
SAVEPOINT s2;
INSERT INTO d1_pk VALUES (30001, 'gone too');
ROLLBACK TO s1;
INSERT INTO d1_pk VALUES (10002, 'kept too');
SELECT id, v FROM d1_pk WHERE id > 5000 ORDER BY id;
COMMIT;
SELECT id, v FROM d1_pk WHERE id > 5000 ORDER BY id;

-- A rolled-back transaction leaves nothing behind.
BEGIN;
INSERT INTO d1_pk VALUES (40001, 'never');
ROLLBACK;
SELECT count(*) FROM d1_pk WHERE id = 40001;

-- New storage is a new tree: TRUNCATE rolls back, and a rewrite keeps
-- the rows.
BEGIN;
TRUNCATE d1_keyless;
SELECT count(*) FROM d1_keyless;
ROLLBACK;
SELECT count(*) FROM d1_keyless;
BEGIN;
TRUNCATE d1_keyless;
INSERT INTO d1_keyless VALUES (7, 'after truncate');
COMMIT;
SELECT * FROM d1_keyless;

-- A rewrite goes through a table without a key, so the tree comes back
-- keyed by content and is rebuilt, keyed by the primary key again, at the
-- next write.  (Reading it as primary-keyed once failed with "invalid
-- canonical prolly value".)
ALTER TABLE d1_pk ALTER COLUMN v TYPE varchar(20);
SELECT count(*), sum(id), max(v) FROM d1_pk;
INSERT INTO d1_pk VALUES (6000, 'after rewrite');
SELECT count(*), sum(id) FROM d1_pk;

-- Adding and dropping a primary key re-keys the tree.
CREATE TABLE d1_rekey (id int, v text);
INSERT INTO d1_rekey SELECT g % 100, 'r' || g FROM generate_series(1, 300) g;
INSERT INTO d1_rekey VALUES (1, 'twin'), (1, 'twin');
ALTER TABLE d1_rekey ADD PRIMARY KEY (id, v);
DROP TABLE d1_rekey;
CREATE TABLE d1_rekey (id int, v text);
INSERT INTO d1_rekey SELECT g, 'r' || g FROM generate_series(1, 300) g;
ALTER TABLE d1_rekey ADD PRIMARY KEY (id);
INSERT INTO d1_rekey VALUES (301, 'r301');
SELECT count(*), count(DISTINCT ctid), sum(id) FROM d1_rekey;
ALTER TABLE d1_rekey DROP CONSTRAINT d1_rekey_pkey;
INSERT INTO d1_rekey VALUES (5, 'r5'), (5, 'r5');
SELECT count(*), count(DISTINCT ctid) FROM d1_rekey WHERE id = 5;
SELECT count(*), sum(id) FROM d1_rekey;

-- A rebuild inside a rolled-back savepoint leaves the transaction's
-- earlier writes alone.
CREATE TABLE d1_sp (id int, v text);
INSERT INTO d1_sp SELECT g, 's' || g FROM generate_series(1, 100) g;
BEGIN;
INSERT INTO d1_sp VALUES (1000, 'before');
ALTER TABLE d1_sp ADD PRIMARY KEY (id);
SAVEPOINT s;
INSERT INTO d1_sp VALUES (1001, 'in savepoint');
ROLLBACK TO s;
SELECT count(*), sum(id) FROM d1_sp;
INSERT INTO d1_sp VALUES (1002, 'after');
COMMIT;
SELECT count(*), sum(id) FROM d1_sp;
INSERT INTO d1_sp VALUES (1003, 'next');
SELECT count(*), sum(id) FROM d1_sp;

-- Columns dropped without a rewrite: rows written before still read (once
-- failed with "invalid canonical prolly value"), the next write rebuilds
-- the tree without the dropped values, and rows keep their TIDs.  Columns
-- added later read as their default.
CREATE TABLE d1_cols (id int, gone text, c int);
INSERT INTO d1_cols SELECT g, 'gone' || g, g % 3 FROM generate_series(1, 6) g;
CREATE TEMP TABLE d1_tids USING heap AS SELECT id, ctid AS tid FROM d1_cols;
ALTER TABLE d1_cols DROP COLUMN gone;
SELECT * FROM d1_cols ORDER BY id;
ALTER TABLE d1_cols ADD COLUMN d int DEFAULT 5;
SELECT * FROM d1_cols ORDER BY id;
INSERT INTO d1_cols VALUES (7, 1, 7);
SELECT * FROM d1_cols ORDER BY id;
SELECT count(*) AS same_tid FROM d1_cols c JOIN d1_tids t ON t.id = c.id
WHERE c.ctid = t.tid;
-- Dropping the column that told rows apart makes them identical.
CREATE TABLE d1_twins (id int, tag text);
INSERT INTO d1_twins VALUES (1, 'a'), (1, 'b'), (2, 'c');
CREATE TEMP TABLE d1_twin_tids USING heap AS SELECT ctid AS tid FROM d1_twins;
ALTER TABLE d1_twins DROP COLUMN tag;
INSERT INTO d1_twins VALUES (3);
SELECT id, count(*), count(DISTINCT ctid) FROM d1_twins GROUP BY id ORDER BY id;
SELECT count(*) AS same_tid FROM d1_twins WHERE ctid IN (SELECT tid FROM d1_twin_tids);
DROP TABLE d1_cols, d1_twins;

-- A table created and filled in one transaction.
BEGIN;
CREATE TABLE d1_new (n int);
INSERT INTO d1_new SELECT generate_series(1, 10);
SELECT sum(n) FROM d1_new;
COMMIT;
SELECT sum(n) FROM d1_new;

-- Indexes are built from a scan of the table.
CREATE INDEX d1_pk_v ON d1_pk (v);
SELECT reltuples >= 0 AS has_stats FROM pg_class WHERE relname = 'd1_pk_v';

-- Scans stay in the leader, even when parallel query is forced: workers
-- can't see the leader's private writes.
BEGIN;
INSERT INTO d1_new VALUES (100);
SET LOCAL debug_parallel_query = on;
SET LOCAL parallel_setup_cost = 0;
SET LOCAL parallel_tuple_cost = 0;
SET LOCAL min_parallel_table_scan_size = 0;
EXPLAIN (COSTS OFF) SELECT sum(n) FROM d1_new;
SELECT sum(n) FROM d1_new;
COMMIT;

-- Committed rows are seen by a new session.
\c
SELECT count(*) FROM d1_pk;
SELECT sum(n) FROM d1_new;

DROP TABLE d1_keyless, d1_pk, d1_new, d1_rekey, d1_sp;
