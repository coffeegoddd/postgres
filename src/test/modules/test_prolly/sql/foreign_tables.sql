-- A foreign table in a versioned schema: a commit holds its definition
-- (columns with their options, CHECK constraints, server and options),
-- never its rows, which are its server's.  Moves make, change and drop it;
-- diffs and merges see its definition; a clone needs its server, which
-- holds connection settings a commit doesn't, made first.  Writes through it
-- change nothing a commit holds.  Not yet: foreign tables that are
-- partitions, inherit, or have triggers or row security.  (In a database of
-- its own; its rows come from an ordinary schema there, over postgres_fdw.)

SELECT current_database() AS db \gset
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/foreign-' || txid_current() AS url \gset
CREATE DATABASE regress_foreign_tables;
\c regress_foreign_tables
CREATE EXTENSION postgres_fdw;
DO $d$ BEGIN
	EXECUTE $$CREATE SERVER loop FOREIGN DATA WRAPPER postgres_fdw
	  OPTIONS (dbname '$$ || current_database() || $$', port '$$ || current_setting('port') || $$')$$;
END $d$;
CREATE USER MAPPING FOR CURRENT_USER SERVER loop;
CREATE SCHEMA remote;
CREATE TABLE remote.src (id int, v text);
CREATE TABLE remote.src2 (id int, v text);
INSERT INTO remote.src VALUES (1, 'one'), (2, 'two');
INSERT INTO remote.src2 VALUES (10, 'ten');

CREATE SCHEMA ft VERSIONED;
CREATE TABLE ft.base (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_hashof('HEAD') AS base \gset
CREATE FOREIGN TABLE ft.f (
	id int OPTIONS (column_name 'id') NOT NULL,
	v text COLLATE "C" DEFAULT 'none',
	CONSTRAINT pos CHECK (id > 0))
  SERVER loop OPTIONS (schema_name 'remote', table_name 'src');
CREATE VIEW ft.fv AS SELECT id, upper(v) AS v FROM ft.f;
SELECT * FROM ft.fv ORDER BY id;
SELECT table_name, status FROM dolt_status() ORDER BY 1;
SELECT length(dolt_commit('-Am', 'foreign table')::text) AS committed;
SELECT dolt_hashof('HEAD') AS with_foreign \gset
SELECT from_create_statement IS NULL AS made, to_create_statement
  FROM dolt_schema_diff(:'base', :'with_foreign', 'ft.f');

-- a branch points it elsewhere; main still reads src
SELECT dolt_checkout('-b', 'ft_b');
ALTER FOREIGN TABLE ft.f OPTIONS (SET table_name 'src2');
SELECT * FROM ft.fv ORDER BY id;
SELECT table_name, status FROM dolt_status() ORDER BY 1;
SELECT length(dolt_commit('-Am', 'src2')::text) AS committed;
SELECT dolt_checkout('main');
SELECT * FROM ft.fv ORDER BY id;
SELECT length(dolt_merge('ft_b')::text) > 0 AS merged;
SELECT * FROM ft.fv ORDER BY id;
SELECT ftoptions FROM pg_foreign_table WHERE ftrelid = 'ft.f'::regclass;

-- both sides change it differently: a schema conflict
SELECT dolt_checkout('-b', 'ft_c');
ALTER FOREIGN TABLE ft.f OPTIONS (SET table_name 'src');
SELECT length(dolt_commit('-Am', 'back to src')::text) AS committed;
SELECT dolt_checkout('main');
ALTER FOREIGN TABLE ft.f ADD COLUMN extra int;
SELECT length(dolt_commit('-Am', 'a column')::text) AS committed;
SELECT dolt_merge('ft_c');

-- moves: back to before it, and forward again (the view with it)
SELECT dolt_hashof('HEAD') AS tip \gset
SELECT dolt_reset('--hard', :'base');
SELECT to_regclass('ft.f') AS f, to_regclass('ft.fv') AS fv;
SELECT dolt_reset('--hard', :'tip');
SELECT id, v FROM ft.fv ORDER BY id;
\d ft.f

-- writes through it change nothing a commit holds
ALTER TABLE remote.src2 ADD COLUMN extra int;
SET dolt.transaction_commit = on;
SELECT count(*) AS commits_before FROM dolt_log;
INSERT INTO ft.f (id, v) VALUES (11, 'eleven');
SELECT count(*) AS commits_after_write FROM dolt_log;
CREATE FOREIGN TABLE ft.g (id int) SERVER loop OPTIONS (schema_name 'remote', table_name 'src');
SELECT count(*) AS commits_after_create FROM dolt_log;
SELECT message FROM dolt_log ORDER BY commit_order DESC LIMIT 1;
RESET dolt.transaction_commit;
SELECT * FROM remote.src2 ORDER BY id;

-- not yet
CREATE FUNCTION remote.noop() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$;
CREATE TRIGGER noop BEFORE INSERT ON ft.g FOR EACH ROW EXECUTE FUNCTION remote.noop();
SELECT dolt_commit('-Am', 'a trigger');
DROP TRIGGER noop ON ft.g;

-- a clone needs the server, made first
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_push('origin', 'main');
\c :db
CREATE DATABASE regress_foreign_clone;
\c regress_foreign_clone
SELECT dolt_clone(:'url');
CREATE EXTENSION postgres_fdw;
DO $d$ BEGIN
	EXECUTE $$CREATE SERVER loop FOREIGN DATA WRAPPER postgres_fdw
	  OPTIONS (dbname 'regress_foreign_tables', port '$$ || current_setting('port') || $$')$$;
END $d$;
CREATE USER MAPPING FOR CURRENT_USER SERVER loop;
SELECT dolt_clone(:'url');
SELECT id, v FROM ft.fv ORDER BY id;

\c :db
DROP DATABASE regress_foreign_clone;
DROP DATABASE regress_foreign_tables;
