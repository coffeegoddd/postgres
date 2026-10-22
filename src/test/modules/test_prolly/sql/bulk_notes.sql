-- A bulk write stops being noted: past 65536 distinct entries of a table's
-- log in a transaction (or a quarter of its rows, if more), its entries are
-- dropped for one saying to read the table in full, and the writes after
-- aren't noted; a rollback to a savepoint before takes it back.  A key
-- written many times is one entry.  A table made in the transaction isn't
-- noted at all.
CREATE DATABASE regress_bulk_notes;
\c regress_bulk_notes
CREATE EXTENSION test_prolly;
CREATE FUNCTION entries(t regclass, OUT n int8, OUT full_read bool) AS $$
	SELECT n, full_read FROM test_log_entries(t)
$$ LANGUAGE sql;
CREATE FUNCTION log_rows(t regclass) RETURNS int8 LANGUAGE plpgsql AS $$
DECLARE
	n int8;
BEGIN
	EXECUTE format('SELECT count(*) FROM pg_toast.pg_prolly_log_%s', t::oid) INTO n;
	RETURN n;
END $$;
CREATE FUNCTION full_folds() RETURNS TABLE (reason text, n bigint) LANGUAGE sql AS $$
	SELECT substr(name, 11), value FROM pg_stat_prolly
	WHERE name LIKE 'fold\_full\_%' AND name <> 'fold_full_tables' AND value > 0
	ORDER BY name
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
CREATE SCHEMA bn VERSIONED;
CREATE TABLE bn.t (id int PRIMARY KEY, v text);
INSERT INTO bn.t SELECT g, 'v' FROM generate_series(1, 1000) g;
SELECT length(dolt_commit('-Am', 'loaded')::text) AS committed;

-- under the limit: noted
BEGIN;
INSERT INTO bn.t SELECT g, 'w' FROM generate_series(1001, 61000) g;
SELECT * FROM entries('bn.t');
ROLLBACK;

-- past it: an entry to read in full, the writes after not noted (those
-- the transaction had written to the log before stay: harmless)
:reset
BEGIN;
INSERT INTO bn.t SELECT g, 'w' FROM generate_series(1001, 71000) g;
SELECT * FROM entries('bn.t');
UPDATE bn.t SET v = 'after' WHERE id = 1;
SELECT same_as_full() AS bulk;
COMMIT;
SELECT length(dolt_commit('-am', 'bulk')::text) AS committed;
:flush
SELECT * FROM full_folds();

-- rolled back to a savepoint before it: noted again, from the baseline
:reset
BEGIN;
UPDATE bn.t SET v = 'kept' WHERE id = 2;
SAVEPOINT s;
INSERT INTO bn.t SELECT g, 'x' FROM generate_series(100001, 170000) g;
SELECT (entries('bn.t')).full_read AS switched;
ROLLBACK TO SAVEPOINT s;
SELECT * FROM entries('bn.t');
INSERT INTO bn.t VALUES (200000, 'kept too');
SELECT * FROM entries('bn.t');
SELECT same_as_full() AS rolled_back;
COMMIT;
SELECT length(dolt_commit('-am', 'rolled back')::text) AS committed;
:flush
SELECT * FROM full_folds();

-- ... and one released keeps it
BEGIN;
SAVEPOINT s;
INSERT INTO bn.t SELECT g, 'y' FROM generate_series(300001, 370000) g;
RELEASE SAVEPOINT s;
DELETE FROM bn.t WHERE id = 3;
SELECT * FROM entries('bn.t');
SELECT same_as_full() AS released;
COMMIT;

-- a fold that begins a baseline in the transaction consumes the switch:
-- the writes after are noted again
BEGIN;
INSERT INTO bn.t SELECT g, 'z' FROM generate_series(400001, 470000) g;
SELECT length(dolt_commit('-am', 'bulk, folded')::text) AS committed;
UPDATE bn.t SET v = 'after the fold' WHERE id = 4;
SELECT * FROM entries('bn.t');
SELECT count(*) AS changes FROM dolt_status;
SELECT same_as_full() AS noted_again;
COMMIT;

-- a table made in the transaction, loaded by COPY: not noted
BEGIN;
CREATE TABLE bn.c (id int PRIMARY KEY, v text);
COPY bn.c FROM stdin;
1	a
2	b
\.
SELECT * FROM entries('bn.c');
COMMIT;
SELECT log_rows('bn.c') AS c_log_rows;
SELECT length(dolt_commit('-Am', 'made')::text) AS committed;
INSERT INTO bn.c VALUES (3, 'c');
SELECT * FROM entries('bn.c');
SELECT same_as_full() AS made;

-- the same keys written past the limit's count of times: an entry each,
-- noted (66000 writes, 1000 entries)
CREATE TABLE bn.h (id int PRIMARY KEY, v text);
INSERT INTO bn.h SELECT g, 'v' FROM generate_series(1, 1000) g;
SELECT length(dolt_commit('-Am', 'hot')::text) AS committed;
BEGIN;
DO $$ BEGIN FOR i IN 1..66 LOOP UPDATE bn.h SET v = 'hot' || i; END LOOP; END $$;
SELECT * FROM entries('bn.h');
SELECT same_as_full() AS hot;
COMMIT;

-- a quarter of a table never vacuumed or analyzed (no reltuples): its rows
-- counted from its pages, so 70000 entries of 300000 rows' table are
-- noted, and 85000, past a quarter, aren't
CREATE TABLE bn.e (id int PRIMARY KEY, v text);
INSERT INTO bn.e SELECT g, 'v' FROM generate_series(1, 300000) g;
SELECT length(dolt_commit('-Am', 'estimated')::text) AS committed;
SELECT reltuples < 0 AS never_counted FROM pg_class WHERE oid = 'bn.e'::regclass;
BEGIN;
UPDATE bn.e SET v = 'w' WHERE id <= 70000;
SELECT * FROM entries('bn.e');
SELECT same_as_full() AS quarter_under;
ROLLBACK;
BEGIN;
UPDATE bn.e SET v = 'w' WHERE id <= 85000;
SELECT (entries('bn.e')).full_read AS quarter_past;
ROLLBACK;

\c contrib_regression
DROP DATABASE regress_bulk_notes;
