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

-- A renamed key keeps its identity, so older rows read it under its name
-- now: every old row is matched by its key, even when the projected rows
-- are identical (#653).
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
-- The historical versions name the key otherwise (pk and pk2), neither
-- as now: it is the same column by identity, so its rows match by key.
ALTER TABLE dh.rpk RENAME COLUMN pk2 TO pk3;
SELECT diff_type, from_row, to_row
FROM dolt_diff(:'before_rename', 'HEAD', NULL::dh.rpk)
ORDER BY diff_type, (from_row).v, (to_row).v;

SELECT diff_type, from_row, to_row
FROM dolt_diff('HEAD', :'before_rename', NULL::dh.rpk)
ORDER BY diff_type, (from_row).v, (to_row).v;

-- A key made of a column older versions lack: their rows have no current
-- key, and each must survive, removed or added, even when the projected
-- rows are identical (#653).
CREATE TABLE dh.newkey (pk int PRIMARY KEY, v int);
INSERT INTO dh.newkey VALUES (1, 7), (2, 7), (3, 7);
SELECT length(dolt_commit('-Am', 'newkey base')::text) AS committed;
SELECT dolt_hashof('HEAD') AS newkey_base \gset
ALTER TABLE dh.newkey ADD COLUMN k int;
UPDATE dh.newkey SET k = pk * 10;
ALTER TABLE dh.newkey DROP CONSTRAINT newkey_pkey;
ALTER TABLE dh.newkey DROP COLUMN pk;
ALTER TABLE dh.newkey ADD PRIMARY KEY (k);
UPDATE dh.newkey SET v = 9 WHERE k = 10;
SELECT length(dolt_commit('-am', 'newkey keyed anew')::text) AS committed;
SELECT diff_type, from_row, to_row
FROM dolt_diff(:'newkey_base', 'HEAD', NULL::dh.newkey)
ORDER BY diff_type, (to_row).k, (from_row).v;
SELECT diff_type, from_row, to_row
FROM dolt_diff('HEAD', :'newkey_base', NULL::dh.newkey)
ORDER BY diff_type, (from_row).k, (to_row).v;
SELECT * FROM dolt_at(:'newkey_base', NULL::dh.newkey) ORDER BY v;

-- Both commits predate a key made anew (of a column they lack) and share
-- their old definition.  Match stored primary keys: the edit is modified,
-- while the inserted and deleted keys remain added/removed.  Partial
-- composite keys work too.
CREATE TABLE dh.oldpk (pk int PRIMARY KEY, v int);
CREATE TABLE dh.oldcpk (a int, b int, v int, PRIMARY KEY (a, b));
INSERT INTO dh.oldpk VALUES (1, 7), (2, 7), (3, 8), (4, 8);
INSERT INTO dh.oldcpk VALUES (1, 1, 7), (1, 2, 7), (2, 1, 8);
SELECT length(dolt_commit('-Am', 'old keys base')::text) AS committed;
SELECT dolt_hashof('HEAD') AS old_keys_base \gset
UPDATE dh.oldpk SET v = 9 WHERE pk = 1;
DELETE FROM dh.oldpk WHERE pk = 3;
INSERT INTO dh.oldpk VALUES (5, 9);
UPDATE dh.oldcpk SET v = 9 WHERE a = 1 AND b = 1;
SELECT length(dolt_commit('-am', 'old keys changed')::text) AS committed;
SELECT dolt_hashof('HEAD') AS old_keys_changed \gset
ALTER TABLE dh.oldpk ADD COLUMN id int;
UPDATE dh.oldpk SET id = pk;
ALTER TABLE dh.oldpk DROP CONSTRAINT oldpk_pkey;
ALTER TABLE dh.oldpk DROP COLUMN pk;
ALTER TABLE dh.oldpk ADD PRIMARY KEY (id);
ALTER TABLE dh.oldcpk ADD COLUMN b2 int;
UPDATE dh.oldcpk SET b2 = b;
ALTER TABLE dh.oldcpk DROP CONSTRAINT oldcpk_pkey;
ALTER TABLE dh.oldcpk DROP COLUMN b;
ALTER TABLE dh.oldcpk ADD PRIMARY KEY (a, b2);
SELECT length(dolt_commit('-am', 'old keys made anew')::text) AS committed;
SELECT diff_type, from_row, to_row
FROM dolt_diff(:'old_keys_base', :'old_keys_changed', NULL::dh.oldpk)
ORDER BY diff_type, (from_row).v, (to_row).v;
SELECT diff_type, from_row, to_row
FROM dolt_diff(:'old_keys_base', :'old_keys_changed', NULL::dh.oldcpk)
ORDER BY diff_type;
SELECT table_name, rows_added, rows_deleted, rows_modified
FROM dolt_diff_stat(:'old_keys_base', :'old_keys_changed', 'dh.oldpk');
SELECT diff_type, from_row, to_row
FROM dolt_diff(:'old_keys_changed', :'old_keys_base', NULL::dh.oldpk)
ORDER BY diff_type, (from_row).v, (to_row).v;
SELECT diff_type, count(*)
FROM dolt_diff_table(NULL::dh.oldpk, :'old_keys_base', :'old_keys_changed')
GROUP BY diff_type ORDER BY diff_type;

-- Keyless-origin counts cancel without expanding copies into sort entries.
-- Distinct values that both fit to NULL must not cancel each other.
CREATE TABLE dh.oldbag (v varchar(20));
INSERT INTO dh.oldbag VALUES ('same'), ('same'), ('same'), ('too long old');
SELECT length(dolt_commit('-Am', 'old bag base')::text) AS committed;
SELECT dolt_hashof('HEAD') AS old_bag_base \gset
DELETE FROM dh.oldbag WHERE ctid IN (SELECT ctid FROM dh.oldbag WHERE v = 'same' LIMIT 1);
UPDATE dh.oldbag SET v = 'too long new' WHERE v = 'too long old';
SELECT length(dolt_commit('-am', 'old bag changed')::text) AS committed;
SELECT dolt_hashof('HEAD') AS old_bag_changed \gset
ALTER TABLE dh.oldbag ADD COLUMN id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY;
ALTER TABLE dh.oldbag ALTER COLUMN v TYPE varchar(4) USING left(v, 4);
SELECT diff_type, from_row, to_row
FROM dolt_diff(:'old_bag_base', :'old_bag_changed', NULL::dh.oldbag)
ORDER BY diff_type, (from_row).v;
SELECT diff_type, from_row, to_row
FROM dolt_diff(:'old_bag_changed', :'old_bag_base', NULL::dh.oldbag)
ORDER BY diff_type, (to_row).v;

-- A whole historical table can lack the current key: sort within work_mem.
CREATE TABLE dh.spillpk (pk int PRIMARY KEY, v text);
INSERT INTO dh.spillpk SELECT g, repeat(md5(g::text), 8) FROM generate_series(1, 1000) g;
SELECT length(dolt_commit('-Am', 'spill base')::text) AS committed;
SELECT dolt_hashof('HEAD') AS spill_base \gset
UPDATE dh.spillpk SET v = 'changed' WHERE pk = 1;
SELECT length(dolt_commit('-am', 'spill changed')::text) AS committed;
SELECT dolt_hashof('HEAD') AS spill_changed \gset
ALTER TABLE dh.spillpk ADD COLUMN id int;
UPDATE dh.spillpk SET id = pk;
ALTER TABLE dh.spillpk DROP CONSTRAINT spillpk_pkey;
ALTER TABLE dh.spillpk DROP COLUMN pk;
ALTER TABLE dh.spillpk ADD PRIMARY KEY (id);
SET work_mem = '64kB';
SELECT diff_type, count(*) FROM dolt_diff(:'spill_base', :'spill_changed', NULL::dh.spillpk)
GROUP BY diff_type ORDER BY diff_type;
RESET work_mem;

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

-- A change to a column both historical versions had must remain visible
-- after that column is renamed again (#654): it is one column by identity.
CREATE TABLE dh.hc (pk int PRIMARY KEY, c2 int, note text);
INSERT INTO dh.hc VALUES (1, -1, 'same'), (2, -2, 'same'), (3, NULL, 'same');
SELECT length(dolt_commit('-Am', 'hc initial')::text) AS committed;
ALTER TABLE dh.hc RENAME COLUMN c2 TO c3;
SELECT length(dolt_commit('-am', 'hc renamed')::text) AS committed;
SELECT dolt_hashof('HEAD') AS hc_before \gset
UPDATE dh.hc SET c3 = 1 WHERE pk = 1;
UPDATE dh.hc SET c3 = 0 WHERE pk = 3;
SELECT length(dolt_commit('-am', 'hc changed')::text) AS committed;
SELECT dolt_hashof('HEAD') AS hc_after \gset
ALTER TABLE dh.hc RENAME COLUMN c3 TO c2;
SELECT length(dolt_commit('-am', 'hc renamed back')::text) AS committed;
SELECT diff_type, from_row, to_row
FROM dolt_diff(:'hc_before', :'hc_after', NULL::dh.hc) ORDER BY (to_row).pk;
SELECT diff_type, from_row, to_row
FROM dolt_diff(:'hc_after', :'hc_before', NULL::dh.hc) ORDER BY (to_row).pk;
SELECT diff_type, from_row, to_row
FROM dolt_diff_table(NULL::dh.hc, :'hc_before', :'hc_after') ORDER BY (to_row).pk;
-- Different historical layouts: an added NULL column must not turn the
-- unchanged rows into modifications; the dropped column still compares.
ALTER TABLE dh.hc ADD COLUMN spare int;
UPDATE dh.hc SET note = 'changed' WHERE pk = 1;
SELECT length(dolt_commit('-am', 'hc another layout')::text) AS committed;
SELECT dolt_hashof('HEAD') AS hc_layout \gset
ALTER TABLE dh.hc DROP COLUMN note;
SELECT diff_type, from_row, to_row
FROM dolt_diff('HEAD~', :'hc_layout', NULL::dh.hc) ORDER BY (to_row).pk;
-- Keyless rows are identified by content before projection, including
-- hidden values: preserve changed copies and exclude unchanged ones.
CREATE TABLE dh.hk (v int);
INSERT INTO dh.hk VALUES (1), (1), (2);
CREATE TABLE dh.hl (a numeric, b int, v text, visible varchar(20), PRIMARY KEY (a, b));
INSERT INTO dh.hl SELECT 1, g, repeat(md5(g::text), 1000), 'too long' FROM generate_series(1, 2) g;
SELECT length(dolt_commit('-Am', 'hidden values')::text) AS committed;
SELECT dolt_hashof('HEAD') AS hidden_before \gset
UPDATE dh.hk SET v = 3 WHERE v = 1;
UPDATE dh.hl SET v = repeat(md5('changed'), 1000) WHERE b = 1;
SELECT length(dolt_commit('-am', 'hidden values changed')::text) AS committed;
SELECT dolt_hashof('HEAD') AS hidden_after \gset
ALTER TABLE dh.hk ADD COLUMN w int;
ALTER TABLE dh.hk DROP COLUMN v;
ALTER TABLE dh.hl DROP COLUMN v;
-- Fitting visible values and comparing hidden values work together.
ALTER TABLE dh.hl ALTER COLUMN visible TYPE varchar(3) USING left(visible, 3);
SELECT diff_type, from_row, to_row
FROM dolt_diff(:'hidden_before', :'hidden_after', NULL::dh.hk) ORDER BY diff_type;
SELECT diff_type, from_row, to_row
FROM dolt_diff(:'hidden_before', :'hidden_after', NULL::dh.hl) ORDER BY (to_row).b;
-- The same stored composite key also matches when a current key component
-- is absent.  A hidden edit is modified even with identical displayed rows.
ALTER TABLE dh.hl ADD COLUMN b_now int;
UPDATE dh.hl SET b_now = b;
ALTER TABLE dh.hl DROP CONSTRAINT hl_pkey;
ALTER TABLE dh.hl DROP COLUMN b;
ALTER TABLE dh.hl ADD PRIMARY KEY (a, b_now);
SELECT diff_type, (from_row).a AS from_a, (to_row).a AS to_a,
       (from_row).b_now IS NULL AND (to_row).b_now IS NULL AS missing_key,
       from_row IS NOT DISTINCT FROM to_row AS same_display
FROM dolt_diff(:'hidden_before', :'hidden_after', NULL::dh.hl);
-- Ordinary historical reads still return every projected row, once.
SELECT count(*) FROM dolt_at(:'hidden_before', NULL::dh.hk);

-- Hidden columns are matched by historical names, not positions.  A
-- wider typmod alone does not change an otherwise identical value.
CREATE TABLE dh.ho (pk int PRIMARY KEY, x varchar(10), y int);
INSERT INTO dh.ho VALUES (1, 'one', 10), (2, 'two', 20);
SELECT length(dolt_commit('-Am', 'hidden column order')::text) AS committed;
SELECT dolt_hashof('HEAD') AS order_before \gset
ALTER TABLE dh.ho DROP COLUMN x;
ALTER TABLE dh.ho ADD COLUMN x varchar(20);
UPDATE dh.ho SET x = CASE pk WHEN 1 THEN 'changed' ELSE 'two' END;
SELECT length(dolt_commit('-am', 'hidden column order changed')::text) AS committed;
ALTER TABLE dh.ho ADD COLUMN x_now varchar(20), ADD COLUMN y_now int;
ALTER TABLE dh.ho DROP COLUMN x, DROP COLUMN y;
SELECT diff_type, from_row, to_row
FROM dolt_diff(:'order_before', 'HEAD', NULL::dh.ho) ORDER BY (to_row).pk;

\c contrib_regression
DROP DATABASE regress_diff_history;
