-- A fold that reads its table in full says why: counted in pg_stat_prolly
-- by reason, logged past prolly.log_full_folds rows; and an incremental
-- fold is the same as a full one after each kind of DDL
-- (debug_prolly_force_full_fold makes the fold read every row).
CREATE DATABASE regress_full_folds;
\c regress_full_folds
CREATE EXTENSION test_prolly;
CREATE FUNCTION full_folds() RETURNS TABLE (reason text, n bigint) LANGUAGE sql AS $$
	SELECT substr(name, 11), value FROM pg_stat_prolly
	WHERE name LIKE 'fold\_full\_%' AND name <> 'fold_full_tables' AND value > 0
	ORDER BY name
$$;
\set reset 'SELECT pg_stat_force_next_flush(); SELECT pg_stat_reset_shared(''prolly'');'
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE SCHEMA ff VERSIONED;
CREATE TABLE ff.t (id int PRIMARY KEY, v text);
INSERT INTO ff.t SELECT g, 'v' || g FROM generate_series(1, 1000) g;

-- the first fold: no baseline
:reset
SELECT length(dolt_commit('-Am', 'first')::text) AS committed;
:flush
SELECT * FROM full_folds();

-- a row changed: from the baseline, none in full
:reset
UPDATE ff.t SET v = 'x' WHERE id = 1;
SELECT length(dolt_commit('-am', 'one row')::text) AS committed;
:flush
SELECT * FROM full_folds();

-- its definition changed
:reset
ALTER TABLE ff.t ADD COLUMN w int DEFAULT 7;
SELECT length(dolt_commit('-am', 'w added')::text) AS committed;
:flush
SELECT * FROM full_folds();

-- truncated and filled again: the log's entry says to
:reset
TRUNCATE ff.t;
INSERT INTO ff.t SELECT g, 'again', 1 FROM generate_series(1, 500) g;
SELECT length(dolt_commit('-am', 'reloaded')::text) AS committed;
:flush
SELECT * FROM full_folds();

-- forced (logged, past prolly.log_full_folds rows: t/127_full_fold_log)
:reset
SET debug_prolly_force_full_fold = on;
SELECT count(*) AS changes FROM dolt_status;
RESET debug_prolly_force_full_fold;
:flush
SELECT * FROM full_folds();

-- each kind of DDL: the incremental fold's root is the full one's
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
CREATE TABLE ff.m (id int PRIMARY KEY, a int, b text);
INSERT INTO ff.m SELECT g, g % 10, 'b' || g FROM generate_series(1, 2000) g;
SELECT length(dolt_commit('-Am', 'matrix')::text) AS committed;
CREATE INDEX m_a ON ff.m (a);
SELECT same_as_full() AS create_index;
ALTER TABLE ff.m ALTER COLUMN a SET STATISTICS 50;
SELECT same_as_full() AS set_statistics;
ALTER TABLE ff.m ADD CONSTRAINT a_small CHECK (a < 100);
SELECT same_as_full() AS add_check;
ALTER TABLE ff.m ADD COLUMN c int;
SELECT same_as_full() AS add_col_null;
ALTER TABLE ff.m ADD COLUMN d int DEFAULT 3;
SELECT same_as_full() AS add_col_default;
ALTER TABLE ff.m DROP COLUMN c;
SELECT same_as_full() AS drop_col;
ALTER TABLE ff.m ALTER COLUMN a TYPE bigint;
SELECT same_as_full() AS alter_type;
UPDATE ff.m SET b = b || '!' WHERE id % 7 = 0;
SELECT same_as_full() AS update_rows;
DELETE FROM ff.m WHERE id % 11 = 0;
SELECT same_as_full() AS delete_rows;
SELECT length(dolt_commit('-am', 'matrix changed')::text) AS committed;
VACUUM FULL ff.m;
SELECT same_as_full() AS vacuum_full;
CLUSTER ff.m USING m_pkey;
SELECT same_as_full() AS cluster;
TRUNCATE ff.m;
INSERT INTO ff.m (id, a, b) SELECT g, 1, 'r' FROM generate_series(1, 300) g;
SELECT same_as_full() AS truncate_reload;

\c contrib_regression
DROP DATABASE regress_full_folds;
