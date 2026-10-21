-- A truncation leaves an entry saying its transaction emptied the table: a
-- fold of a keyed table that reads it starts from no rows, each key the log
-- names (before or after) looked up in the heap, not the table read in full
-- (a reload costs the rows loaded); a keyless table is read in full, as
-- many rows as were loaded.
CREATE DATABASE regress_truncate_folds;
\c regress_truncate_folds
CREATE EXTENSION test_prolly;
CREATE FUNCTION folds(OUT full_tables bigint, OUT rows_read bigint) LANGUAGE sql AS $$
	SELECT sum(value) FILTER (WHERE name = 'fold_full_tables'),
		sum(value) FILTER (WHERE name = 'fold_rows_read')
	FROM pg_stat_prolly
$$;
CREATE FUNCTION same_as_full() RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	inc text;
	full_root text;
BEGIN
	inc := test_fold_root();
	SET LOCAL debug_prolly_force_full_fold = on;
	full_root := test_fold_root();
	SET LOCAL debug_prolly_force_full_fold = off;
	RETURN inc = full_root;
END $$;
\set reset 'SELECT pg_stat_force_next_flush(); SELECT pg_stat_reset_shared(''prolly'');'
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE SCHEMA tf VERSIONED;
CREATE TABLE tf.t (id int PRIMARY KEY, v text);
CREATE TABLE tf.u (id int PRIMARY KEY, v text);
CREATE TABLE tf.n (v int);
CREATE TABLE tf.r (id int PRIMARY KEY, t int REFERENCES tf.t);
INSERT INTO tf.t SELECT g, 'v' || g FROM generate_series(1, 5000) g;
INSERT INTO tf.u SELECT g, 'u' FROM generate_series(1, 2000) g;
INSERT INTO tf.n SELECT g % 7 FROM generate_series(1, 2000) g;
INSERT INTO tf.r SELECT g, g FROM generate_series(1, 100) g;
SELECT length(dolt_commit('-Am', 'loaded')::text) AS committed;

-- truncated and reloaded in one transaction, a row changed before: from
-- no rows, reading what was loaded (and the key changed before, found gone)
BEGIN;
UPDATE tf.t SET v = 'before' WHERE id = 1;
TRUNCATE tf.t CASCADE;
INSERT INTO tf.t SELECT g, 'again' FROM generate_series(1, 1000) g;
SELECT same_as_full() AS reloaded;
ROLLBACK;
UPDATE tf.t SET v = 'before' WHERE id = 4999;
:reset
BEGIN;
TRUNCATE tf.t CASCADE;
INSERT INTO tf.t SELECT g, 'again' FROM generate_series(1, 1000) g;
SELECT count(*) AS changes FROM dolt_status;
COMMIT;
SELECT length(dolt_commit('-am', 'reloaded')::text) AS committed;
:flush
SELECT * FROM folds();
SELECT count(*) AS t_rows, (SELECT count(*) FROM tf.r) AS r_rows FROM tf.t;
SELECT count(*) AS changes FROM dolt_status;
DROP TABLE tf.r;

-- truncated in one transaction, loaded in a later one, several at once
:reset
TRUNCATE tf.t, tf.u;
INSERT INTO tf.t SELECT g, 'third' FROM generate_series(1, 300) g;
INSERT INTO tf.u VALUES (1, 'one');
SELECT count(*) AS changes FROM dolt_status;
:flush
SELECT * FROM folds();
SELECT same_as_full() AS later;
SELECT length(dolt_commit('-am', 'later')::text) AS committed;

-- a truncation rolled back to a savepoint: the table as it was, its other
-- writes kept
:reset
BEGIN;
UPDATE tf.t SET v = 'kept' WHERE id = 1;
SAVEPOINT s;
TRUNCATE tf.t;
INSERT INTO tf.t VALUES (9000, 'gone');
ROLLBACK TO SAVEPOINT s;
INSERT INTO tf.t VALUES (9001, 'kept too');
COMMIT;
SELECT length(dolt_commit('-am', 'savepoint')::text) AS committed;
:flush
SELECT * FROM folds();
BEGIN;
UPDATE tf.t SET v = 'kept' WHERE id = 2;
SAVEPOINT s;
TRUNCATE tf.t;
ROLLBACK TO SAVEPOINT s;
SELECT same_as_full() AS rolled_back;
COMMIT;
SELECT string_agg(id || ':' || v, ',' ORDER BY id) AS changed FROM tf.t
WHERE v <> 'third';

-- ... and one released: emptied
BEGIN;
SAVEPOINT s;
TRUNCATE tf.t;
INSERT INTO tf.t VALUES (1, 'only');
RELEASE SAVEPOINT s;
SELECT same_as_full() AS released;
COMMIT;
SELECT length(dolt_commit('-am', 'released')::text) AS committed;
SELECT count(*) AS t_rows FROM tf.t;

-- keyless: read in full
:reset
BEGIN;
TRUNCATE tf.n;
INSERT INTO tf.n SELECT g % 3 FROM generate_series(1, 100) g;
COMMIT;
SELECT length(dolt_commit('-am', 'keyless')::text) AS committed;
:flush
SELECT * FROM folds();
TRUNCATE tf.n;
INSERT INTO tf.n VALUES (1);
SELECT same_as_full() AS keyless;

\c contrib_regression
DROP DATABASE regress_truncate_folds;
