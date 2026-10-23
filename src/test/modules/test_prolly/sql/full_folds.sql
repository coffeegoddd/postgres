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

-- its definition changed, its rows' bytes with it: mapped from the baseline
-- (shape_folds), none read in full
:reset
ALTER TABLE ff.t ADD COLUMN w int DEFAULT 7;
SELECT length(dolt_commit('-am', 'w added')::text) AS committed;
:flush
SELECT * FROM full_folds();

-- truncated and filled again: not in full, from no rows (truncate_folds)
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

-- DDL that changes no row's bytes keeps the baseline: a row changed after
-- it, committed, folds from the baseline, not every row; DDL that changes
-- rows' bytes reads them all
CREATE TABLE ff.p (id int PRIMARY KEY);
INSERT INTO ff.p SELECT g FROM generate_series(1, 10) g;
CREATE TABLE ff.s (id int PRIMARY KEY, a int, b text, e text);
INSERT INTO ff.s SELECT g, g % 10, 'b' || g, 'e' FROM generate_series(1, 2000) g;
SELECT length(dolt_commit('-Am', 'shape')::text) AS committed;
\set after 'UPDATE ff.s SET b = b || ''.'' WHERE id = 1; SELECT same_as_full() AS same; SELECT pg_stat_force_next_flush(); SELECT pg_stat_reset_shared(''prolly''); SELECT length(dolt_commit(''-am'', ''ddl'')::text) > 0 AS committed; SELECT pg_stat_force_next_flush(); SELECT coalesce(string_agg(reason, '',''), ''none'') AS read_in_full FROM full_folds();'
CREATE INDEX s_a ON ff.s (a);
:after
DROP INDEX ff.s_a;
:after
ALTER TABLE ff.s ADD CONSTRAINT s_small CHECK (a < 100);
:after
ALTER TABLE ff.s ADD CONSTRAINT s_p FOREIGN KEY (a) REFERENCES ff.p (id) NOT VALID;
:after
ALTER TABLE ff.s ALTER COLUMN e SET DEFAULT 'd';
:after
ALTER TABLE ff.s SET (fillfactor = 70);
:after
ALTER TABLE ff.s REPLICA IDENTITY FULL;
:after
ALTER TABLE ff.s ALTER COLUMN a SET STATISTICS 50;
:after
ALTER TABLE ff.s ALTER COLUMN b SET STORAGE EXTERNAL;
:after
ALTER TABLE ff.s ALTER COLUMN b SET COMPRESSION pglz;
:after
CREATE STATISTICS ff.s_ab ON a, b FROM ff.s;
:after
CREATE FUNCTION ff.touch() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$;
CREATE TRIGGER s_touch BEFORE UPDATE ON ff.s FOR EACH ROW EXECUTE FUNCTION ff.touch();
:after
CREATE POLICY s_all ON ff.s USING (true);
:after
COMMENT ON TABLE ff.s IS 'shaped';
:after
VACUUM FULL ff.s;
:after
CLUSTER ff.s USING s_pkey;
:after
ALTER TABLE ff.s ADD COLUMN n int;
:after
ALTER TABLE ff.s ADD COLUMN d int DEFAULT 3;
:after
ALTER TABLE ff.s DROP COLUMN n;
:after
ALTER TABLE ff.s ALTER COLUMN a TYPE bigint;
:after
CREATE TYPE ff.mood AS ENUM ('sad', 'ok');
CREATE TABLE ff.moods (id int PRIMARY KEY, m ff.mood);
INSERT INTO ff.moods SELECT g, 'ok' FROM generate_series(1, 100) g;
SELECT length(dolt_commit('-Am', 'moods')::text) > 0 AS committed;
ALTER TYPE ff.mood ADD VALUE 'happy';
UPDATE ff.moods SET m = 'sad' WHERE id = 1;
SELECT same_as_full() AS same;
:reset
SELECT length(dolt_commit('-am', 'enum')::text) > 0 AS committed;
:flush
SELECT coalesce(string_agg(reason, ','), 'none') AS read_in_full FROM full_folds();

-- a full fold's rows are sorted in maintenance_work_mem, spilling past it:
-- the same tree either way
CREATE TABLE ff.big (id int PRIMARY KEY, v text);
INSERT INTO ff.big SELECT g, md5(g::text) FROM generate_series(1, 30000) g;
CREATE TABLE ff.dups (v int);
INSERT INTO ff.dups SELECT g % 100 FROM generate_series(1, 30000) g;
SET debug_prolly_force_full_fold = on;
SELECT test_fold_root() AS roomy \gset
SELECT rows_read, spilled FROM test_last_full_fold();
SET maintenance_work_mem = '64kB';
SELECT test_fold_root() = :'roomy' AS same_tree;
SELECT rows_read, spilled, sort_kb < 4096 AS small FROM test_last_full_fold();
RESET maintenance_work_mem;
RESET debug_prolly_force_full_fold;
:flush
SELECT value > 0 AS counted_spilled FROM pg_stat_prolly WHERE name = 'fold_full_spilled';

-- the changes an incremental fold holds past maintenance_work_mem: sorted,
-- spilling, and folded from the baseline, not in full (spilled_folds)
SELECT length(dolt_commit('-Am', 'big')::text) AS committed;
UPDATE ff.big SET v = 'changed' WHERE id <= 5000;
:reset
SET maintenance_work_mem = '64kB';
SELECT test_fold_root() IS NOT NULL AS folded;
RESET maintenance_work_mem;
:flush
SELECT * FROM full_folds();
SELECT value AS changes_spilled FROM pg_stat_prolly WHERE name = 'fold_changes_spilled';

-- many rows changed: merged with the baseline in one pass, not applied a
-- row at a time, nor read in full; the same tree as a full fold
CREATE TABLE ff.wide (id int PRIMARY KEY, v int);
INSERT INTO ff.wide SELECT g, 0 FROM generate_series(1, 20000) g;
SELECT length(dolt_commit('-Am', 'wide')::text) AS committed;
-- (full reads but those same_as_full() forces)
CREATE FUNCTION merged() RETURNS TABLE (merges bigint, full_reads bigint) LANGUAGE sql AS $$
	SELECT max(value) FILTER (WHERE name = 'fold_merge_builds'),
		max(value) FILTER (WHERE name = 'fold_full_tables') -
		max(value) FILTER (WHERE name = 'fold_full_forced')
	FROM pg_stat_prolly
$$;
UPDATE ff.wide SET v = 1 WHERE id % 100 = 0;
:reset
SELECT same_as_full() AS one_percent;
:flush
SELECT * FROM merged();
SELECT length(dolt_commit('-am', '1%')::text) AS committed;
UPDATE ff.wide SET v = 2 WHERE id % 100 < 15;
DELETE FROM ff.wide WHERE id % 100 = 99;
:reset
SELECT same_as_full() AS fifteen_percent;
:flush
SELECT * FROM merged();
SELECT length(dolt_commit('-am', '15%')::text) AS committed;
UPDATE ff.wide SET v = 3 WHERE id % 100 < 60;
INSERT INTO ff.wide SELECT g, 3 FROM generate_series(20001, 25000) g;
:reset
SELECT same_as_full() AS sixty_percent;
:flush
SELECT * FROM merged();

\c contrib_regression
DROP DATABASE regress_full_folds;
