-- A branch's first use whose head hasn't a table the copied branch has:
-- the table's copy begins empty, its pages not shared, and the move drops
-- it (with what depends on it: a view, the sequence its column owns).
-- Sharing a table's pages logs an image of each page no other map held
-- (as a big table made since the head has), so the first use's WAL
-- followed the dropped table's size; now it doesn't.  A table whose
-- definition differs keeps its pages, as before.
CREATE DATABASE regress_discarded_storage;
\c regress_discarded_storage
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
CREATE EXTENSION amcheck;
RESET client_min_messages;
SET dolt.author = 'Ada Lovelace <ada@example.com>';
CREATE SCHEMA ns VERSIONED;
CREATE TABLE ns.s (id int PRIMARY KEY, v text);
INSERT INTO ns.s SELECT g, 's' || g FROM generate_series(1, 100) g;
SELECT length(dolt_commit('-Am', 'head')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS head \gset

-- main since: a big table (two indexes, a sequence, a view on it), and
-- s given a column.
CREATE TABLE ns.big (id serial PRIMARY KEY, v text, w int);
CREATE INDEX big_v ON ns.big (v);
CREATE INDEX big_w ON ns.big (w);
INSERT INTO ns.big (v, w) SELECT 'v' || g, g % 1000 FROM generate_series(1, 200000) g;
CREATE VIEW ns.big_view AS SELECT id, v FROM ns.big WHERE w = 0;
ALTER TABLE ns.s ADD COLUMN x int DEFAULT 7;
SELECT length(dolt_commit('-Am', 'big')::text) > 0 AS committed;
SELECT pg_relation_size('ns.big') > 8000000 AS big;
SELECT dolt_branch('at_head', :'head');

-- A first use rolled back: main as it was.
BEGIN;
SELECT dolt_checkout('at_head');
ROLLBACK;
SELECT count(*) FROM ns.big;

CHECKPOINT;
SELECT pg_current_wal_insert_lsn() AS lsn \gset
SELECT dolt_checkout('at_head');
SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), :'lsn') < 4000000 AS wal_not_the_tables;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_head;
SELECT count(*) AS changes FROM dolt_status;
SELECT relname FROM pg_class WHERE relnamespace = 'ns'::regnamespace ORDER BY 1;
SELECT count(*) FILTER (WHERE v = 's' || id) AS s_rows FROM ns.s;
SELECT count(*) AS s_columns FROM information_schema.columns
WHERE table_schema = 'ns' AND table_name = 's';

-- main keeps its big table, its pages and indexes its own.
SELECT dolt_checkout('main');
SELECT count(*), count(*) FILTER (WHERE w = 0) AS in_view FROM ns.big;
SELECT count(*) FROM ns.big_view;
SELECT bt_index_check('ns.big_pkey', true), bt_index_check('ns.big_v', true),
	bt_index_check('ns.big_w', true);
SELECT count(*) AS changes FROM dolt_status;
SELECT nextval('ns.big_id_seq');

\c contrib_regression
DROP DATABASE regress_discarded_storage;
