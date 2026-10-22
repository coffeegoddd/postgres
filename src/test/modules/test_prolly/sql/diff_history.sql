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
-- columns narrowed (a shorter varchar; a domain whose check an old value
-- fails): an old value the column couldn't hold now shows as null, with a
-- warning, as one that doesn't convert does (#590); one that fits, kept
-- (1.25, which the ALTER rounded to 1.3 as stored, is a change, though
-- both show 1.3)
CREATE DOMAIN dh.tinyint AS smallint CHECK (VALUE BETWEEN -128 AND 127);
CREATE TABLE dh.nt (pk int PRIMARY KEY, a varchar(20), b int, c int, d numeric(6,2));
INSERT INTO dh.nt VALUES (1, '123456789012345', 70000, 420, 1234.5), (2, 'short', 7, 7, 1.25);
SELECT length(dolt_commit('-Am', 'nt')::text) AS committed;
UPDATE dh.nt SET a = '1234567890', b = 13, c = 13, d = 12.5 WHERE pk = 1;
ALTER TABLE dh.nt ALTER COLUMN a TYPE varchar(10);
ALTER TABLE dh.nt ALTER COLUMN b TYPE smallint;
ALTER TABLE dh.nt ALTER COLUMN c TYPE dh.tinyint;
ALTER TABLE dh.nt ALTER COLUMN d TYPE numeric(4,1);
SELECT length(dolt_commit('-am', 'nt narrowed')::text) AS committed;
SELECT diff_type, from_row, to_row FROM dolt_diff_table(NULL::dh.nt, 'HEAD~1', 'HEAD')
ORDER BY coalesce((to_row).pk, (from_row).pk);
SELECT pk, a, length(a), b, c, d FROM dolt_at('HEAD~1', NULL::dh.nt) ORDER BY pk;
SELECT (row).pk, (row).a, (row).c, c.message
FROM dolt_history_table(NULL::dh.nt) h JOIN dolt_log c ON c.commit_hash = h.commit_hash
ORDER BY c.commit_order DESC, (row).pk;
-- the same values, written now, fail
INSERT INTO dh.nt VALUES (3, '123456789012345');
INSERT INTO dh.nt VALUES (3, NULL, NULL, 420);
-- a change between two old values that both don't fit now (both null, or
-- rounded alike) is still a change: compared as stored, shown fitted; a
-- row that didn't change isn't, though it doesn't fit either (pk 2)
CREATE DOMAIN dh.pos AS int CHECK (VALUE > 0);
CREATE TABLE dh.nf (pk int PRIMARY KEY, v varchar(20), p int, d numeric(6,2));
CREATE TABLE dh.nfk (v varchar(20));
INSERT INTO dh.nf VALUES (1, 'abcdefghijklmno', -1, 1.25),
	(2, 'unchanged and long', -5, 2.25);
INSERT INTO dh.nfk VALUES ('abcdefghijklmno');
SELECT length(dolt_commit('-Am', 'nf')::text) AS committed;
UPDATE dh.nf SET v = 'zyxwvutsrqponml', p = -2, d = 1.26 WHERE pk = 1;
UPDATE dh.nfk SET v = 'zyxwvutsrqponml';
SELECT length(dolt_commit('-am', 'nf changed')::text) AS committed;
UPDATE dh.nf SET v = 'x', p = 1, d = 1;
UPDATE dh.nfk SET v = 'x';
ALTER TABLE dh.nf ALTER COLUMN v TYPE varchar(10);
ALTER TABLE dh.nf ALTER COLUMN p TYPE dh.pos;
ALTER TABLE dh.nf ALTER COLUMN d TYPE numeric(4,1);
ALTER TABLE dh.nfk ALTER COLUMN v TYPE varchar(10);
SELECT length(dolt_commit('-am', 'nf narrowed')::text) AS committed;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~2', 'HEAD~1', NULL::dh.nf);
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~2', 'HEAD~1', NULL::dh.nfk);

-- a primary key dropped, then made on another column: each version read
-- as the table is keyed now, so its rows diff (where Dolt warns and shows
-- what it can)
CREATE TABLE dh.pkc (pk int PRIMARY KEY, c1 int);
INSERT INTO dh.pkc VALUES (1, 2), (3, 4);
SELECT length(dolt_commit('-Am', 'pkc')::text) AS committed;
ALTER TABLE dh.pkc DROP CONSTRAINT pkc_pkey;
INSERT INTO dh.pkc VALUES (5, 6);
SELECT length(dolt_commit('-am', 'pkc dropped the key')::text) AS committed;
ALTER TABLE dh.pkc ADD PRIMARY KEY (c1);
SELECT length(dolt_commit('-am', 'pkc keyed by c1')::text) AS committed;
INSERT INTO dh.pkc VALUES (7, 8);
SELECT length(dolt_commit('-am', 'pkc more')::text) AS committed;
SELECT diff_type, from_row, to_row FROM dolt_diff_table(NULL::dh.pkc)
ORDER BY to_commit IS NULL, (to_row).c1, (from_row).c1;
SELECT count(*) FROM dolt_history_table(NULL::dh.pkc);
SELECT * FROM dolt_at('HEAD~2', NULL::dh.pkc) ORDER BY c1;
SELECT diff_type, data_change, schema_change FROM dolt_diff_summary('HEAD~3', 'HEAD~1', 'dh.pkc');
-- a keyless table given a key; and one whose copies share it: shown once
CREATE TABLE dh.kl (i int, j int);
INSERT INTO dh.kl VALUES (42, 23), (1, 1), (1, 1);
SELECT length(dolt_commit('-Am', 'kl')::text) AS committed;
DELETE FROM dh.kl WHERE i = 1;
INSERT INTO dh.kl VALUES (1, 1);
ALTER TABLE dh.kl ADD PRIMARY KEY (i);
SELECT length(dolt_commit('-am', 'kl keyed')::text) AS committed;
SELECT diff_type, from_row, to_row FROM dolt_diff_table(NULL::dh.kl, 'HEAD~1', 'HEAD');
SELECT * FROM dolt_at('HEAD~1', NULL::dh.kl) ORDER BY i;

-- A renamed key is absent when older rows are read by name.  Every old
-- row must survive, even when the projected rows are identical (#653).
CREATE TABLE dh.rpk (pk int PRIMARY KEY, v int);
INSERT INTO dh.rpk VALUES (1, 7), (2, 7), (3, 7);
CREATE TABLE dh.rcpk (a numeric, b int, v int, PRIMARY KEY (a, b));
INSERT INTO dh.rcpk VALUES (1, 1, 7), (1, 2, 7), (2, 1, 8);
SELECT length(dolt_commit('-Am', 'before key rename')::text) AS committed;
SELECT dolt_hashof('HEAD') AS before_rename \gset
ALTER TABLE dh.rpk RENAME COLUMN pk TO pk2;
ALTER TABLE dh.rcpk RENAME COLUMN b TO b2;
-- A rename alone still changes no stored row.
SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::dh.rpk);
UPDATE dh.rpk SET v = 9 WHERE pk2 = 1;
UPDATE dh.rcpk SET v = 9 WHERE a = 1 AND b2 = 1;
-- WORKING and committed diffs, in both directions.
SELECT diff_type, from_row, to_row
FROM dolt_diff('HEAD', 'WORKING', NULL::dh.rpk)
ORDER BY diff_type, (to_row).pk2, (from_row).v;
SELECT length(dolt_commit('-am', 'after key rename')::text) AS committed;
SELECT diff_type, from_row, to_row
FROM dolt_diff('HEAD~', 'HEAD', NULL::dh.rpk)
ORDER BY diff_type, (to_row).pk2, (from_row).v;
SELECT diff_type, from_row, to_row
FROM dolt_diff('HEAD', 'HEAD~', NULL::dh.rpk)
ORDER BY diff_type, (from_row).pk2, (to_row).v;
SELECT diff_type, from_row, to_row
FROM dolt_diff('HEAD~', 'HEAD', NULL::dh.rcpk)
ORDER BY diff_type, (to_row).a, (to_row).b2, (from_row).a, (from_row).v;
SELECT * FROM dolt_at('HEAD~', NULL::dh.rpk) ORDER BY v;
SELECT diff_type, count(*) FROM dolt_diff_table(NULL::dh.rpk, 'HEAD~', 'HEAD')
GROUP BY diff_type ORDER BY diff_type;
SELECT count(*) FROM dolt_history_table(NULL::dh.rpk);
-- Neither historical version has the current key's name.  They cannot
-- be matched by that key, but all rows must still be reported.
ALTER TABLE dh.rpk RENAME COLUMN pk2 TO pk3;
SELECT diff_type, from_row, to_row
FROM dolt_diff(:'before_rename', 'HEAD', NULL::dh.rpk)
ORDER BY diff_type, (from_row).v, (to_row).v;

-- An absent key added to a keyless version must retain its row counts;
-- old values that cannot fit the current row type still show as NULL.
CREATE TABLE dh.rkl (v varchar(20));
INSERT INTO dh.rkl VALUES ('too long'), ('too long'), ('too long');
SELECT length(dolt_commit('-Am', 'keyless copies')::text) AS committed;
ALTER TABLE dh.rkl ADD COLUMN id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY;
ALTER TABLE dh.rkl ALTER COLUMN v TYPE varchar(3) USING left(v, 3);
SELECT length(dolt_commit('-am', 'copies given keys')::text) AS committed;
SELECT diff_type, from_row, to_row
FROM dolt_diff('HEAD~', 'HEAD', NULL::dh.rkl)
ORDER BY diff_type, (to_row).id;
SELECT * FROM dolt_at('HEAD~', NULL::dh.rkl);

\c contrib_regression
DROP DATABASE regress_diff_history;
