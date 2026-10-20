-- A table's changes over its history, commit by commit (dolt_diff_table,
-- as Dolt's dolt_diff_<table>), and what each commit changed, object by
-- object (the dolt_diff view, as Dolt's dolt_diff system table).
CREATE DATABASE regress_diff_history;
\c regress_diff_history
CREATE EXTENSION test_prolly;
CREATE SCHEMA dh VERSIONED;
CREATE TABLE dh.t (id int PRIMARY KEY, v text);
CREATE TABLE dh.other (id int PRIMARY KEY);
INSERT INTO dh.t VALUES (1, 'one');
SELECT length(dolt_commit('-Am', 'first')::text) AS committed;
SELECT dolt_hashof('HEAD') AS c1 \gset
INSERT INTO dh.t VALUES (2, 'two');
SELECT length(dolt_commit('-Am', 'second')::text) AS committed;
SELECT dolt_hashof('HEAD') AS c2 \gset
-- a commit that doesn't change dh.t
INSERT INTO dh.other VALUES (1);
SELECT length(dolt_commit('-Am', 'other only')::text) AS committed;
SELECT dolt_hashof('HEAD') AS c3 \gset
-- a merge: its first parent's line is the one followed
SELECT dolt_branch('side');
SELECT dolt_checkout('side');
UPDATE dh.t SET v = 'side' WHERE id = 1;
SELECT length(dolt_commit('-am', 'on side')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO dh.t VALUES (3, 'three');
SELECT length(dolt_commit('-am', 'on main')::text) AS committed;
SELECT dolt_merge('side') IS NOT NULL AS merged;
-- a column added: old rows read as the table is defined now
ALTER TABLE dh.t ADD COLUMN w int;
UPDATE dh.t SET w = 9 WHERE id = 2;
SELECT length(dolt_commit('-am', 'w added')::text) AS committed;
-- and the working set
DELETE FROM dh.t WHERE id = 3;

CREATE FUNCTION pg_temp.short(text) RETURNS text LANGUAGE sql AS $$
	SELECT CASE WHEN $1 IS NULL THEN NULL WHEN $1 = 'WORKING' THEN $1
		ELSE (SELECT message FROM dolt_log WHERE commit_hash = $1) END
$$;
SELECT pg_temp.short(to_commit) AS to_commit, pg_temp.short(from_commit) AS from_commit,
	diff_type, from_row, to_row, to_commit_date IS NULL AS working_undated
FROM dolt_diff_table(NULL::dh.t)
ORDER BY to_commit_date DESC NULLS FIRST, (coalesce(to_row, from_row)).id;

-- a range: from (not its changes) to
SELECT pg_temp.short(to_commit) AS to_commit, diff_type, to_row
FROM dolt_diff_table(NULL::dh.t, :'c1', :'c3') ORDER BY to_commit_date DESC;

-- the dolt_diff view: each commit's objects changed, with the commit's
-- message, the working set's first
SELECT commit_hash = 'WORKING' AS working, message, table_name, data_change,
	schema_change
FROM dolt_diff ORDER BY date DESC NULLS FIRST, table_name;

-- a table dropped and made again: its history by name
DROP TABLE dh.other;
SELECT length(dolt_commit('-Am', 'other dropped')::text) AS committed;
CREATE TABLE dh.other (id int PRIMARY KEY, note text);
INSERT INTO dh.other VALUES (5, 'again');
SELECT pg_temp.short(to_commit) AS to_commit, diff_type, from_row, to_row
FROM dolt_diff_table(NULL::dh.other) ORDER BY to_commit_date DESC NULLS FIRST;

SELECT * FROM dolt_diff_table(NULL::dh.t, 'no_such_ref');

-- a column dropped and added again as another type: an old value that
-- doesn't convert to it shows as null, with a warning (as Dolt shows it);
-- one that does, converted; a key column's old values that don't, refused
CREATE TABLE dh.rt (pk int PRIMARY KEY, c varchar(20));
INSERT INTO dh.rt VALUES (1, 'abc'), (2, '7'), (3, 'xyz');
SELECT length(dolt_commit('-Am', 'rt')::text) AS committed;
ALTER TABLE dh.rt DROP COLUMN c;
SELECT length(dolt_commit('-am', 'rt dropped c')::text) AS committed;
ALTER TABLE dh.rt ADD COLUMN c int;
INSERT INTO dh.rt VALUES (4, 9);
SELECT length(dolt_commit('-am', 'rt c again')::text) AS committed;
SELECT count(*) FROM dolt_diff_table(NULL::dh.rt);
SELECT diff_type, from_row, to_row FROM dolt_diff_table(NULL::dh.rt, 'HEAD~3', 'HEAD~2')
ORDER BY coalesce((to_row).pk, (from_row).pk);
SELECT * FROM dolt_at('HEAD~2', NULL::dh.rt) ORDER BY pk;
SELECT count(*) FROM dolt_history_table(NULL::dh.rt);
CREATE TABLE dh.rk (k text PRIMARY KEY);
INSERT INTO dh.rk VALUES ('1'), ('one');
SELECT length(dolt_commit('-Am', 'rk')::text) AS committed;
ALTER TABLE dh.rk ALTER COLUMN k TYPE int USING length(k);
SELECT * FROM dolt_at('HEAD', NULL::dh.rk);

\c contrib_regression
DROP DATABASE regress_diff_history;
