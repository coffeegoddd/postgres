-- A table's rows diffed by its name (dolt_diff, dolt_diff_table and
-- dolt_history_table given the table's name as text): so a table the
-- session hasn't, dropped since or renamed, can be read, each version's
-- rows as jsonb, decoded as that version defines the table (#612).
CREATE DATABASE regress_diff_by_name;
\c regress_diff_by_name
CREATE EXTENSION test_prolly;
CREATE SCHEMA dn VERSIONED;
SET search_path = dn, public;
CREATE TABLE dn.t1 (a int PRIMARY KEY, b int);
INSERT INTO dn.t1 VALUES (1, 2), (3, 4);
SELECT length(dolt_commit('-Am', 'new table')::text) AS committed;

-- a table that exists now: the same rows as its row type gives, as jsonb
UPDATE dn.t1 SET b = 20 WHERE a = 1;
INSERT INTO dn.t1 VALUES (5, 6);
DELETE FROM dn.t1 WHERE a = 3;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', 'dn.t1')
ORDER BY coalesce(to_row, from_row)->>'a';
SELECT count(*) AS same_rows FROM (
	SELECT diff_type, to_jsonb(from_row) AS f, to_jsonb(to_row) AS t
	FROM dolt_diff('HEAD', 'WORKING', NULL::dn.t1)
	INTERSECT
	SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD', 'WORKING', 't1')) s;
SELECT length(dolt_commit('-am', 'changed')::text) AS committed;

-- a column added: each version's rows with its own columns
ALTER TABLE dn.t1 ADD COLUMN c text;
UPDATE dn.t1 SET c = 'x' WHERE a = 5;
SELECT length(dolt_commit('-am', 'c added')::text) AS committed;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~', 'HEAD', 't1')
ORDER BY coalesce(to_row, from_row)->>'a';

-- dropped: its rows removed, between the commits before and after the drop
DROP TABLE dn.t1;
SELECT length(dolt_commit('-am', 'dropped table')::text) AS committed;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~', 'HEAD', 'dn.t1')
ORDER BY from_row->>'a';
SELECT diff_type, to_row FROM dolt_diff('HEAD', 'HEAD~', 'dn.t1')
ORDER BY to_row->>'a';
-- (its history by name, the row-type forms having no type to take)
SELECT diff_type, from_row FROM dolt_diff_table('dn.t1')
WHERE to_commit = dolt_hashof('HEAD') ORDER BY from_row->>'a';
SELECT count(*) AS changes FROM dolt_diff_table('dn.t1');
SELECT c.message, h.row FROM dolt_history_table('dn.t1') h
JOIN dolt_log c ON c.commit_hash = h.commit_hash
ORDER BY c.date, h.row->>'a';

-- renamed, rows changed too (one table, by its identity, as Dolt's tags
-- match it): by its old name or its new, the row added alone
CREATE TABLE dn.r1 (pk int PRIMARY KEY, v text);
INSERT INTO dn.r1 VALUES (1, 'one');
SELECT length(dolt_commit('-Am', 'r1')::text) AS committed;
ALTER TABLE dn.r1 RENAME TO r2;
INSERT INTO dn.r2 VALUES (2, 'two');
SELECT length(dolt_commit('-am', 'renamed')::text) AS committed;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~', 'HEAD', 'r1');
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~', 'HEAD', 'r2')
ORDER BY to_row->>'pk';
-- renamed alone: the same rows, none differ
ALTER TABLE dn.r2 RENAME TO r3;
SELECT length(dolt_commit('-am', 'renamed again')::text) AS committed;
SELECT count(*) FROM dolt_diff('HEAD~', 'HEAD', 'r2');
SELECT count(*) FROM dolt_diff('HEAD~', 'HEAD', 'r3');

-- a keyless table's copies, dropped: each copy a row
CREATE TABLE dn.bag (i int);
INSERT INTO dn.bag VALUES (1), (1), (2);
SELECT length(dolt_commit('-Am', 'bag')::text) AS committed;
DROP TABLE dn.bag;
SELECT length(dolt_commit('-am', 'bag dropped')::text) AS committed;
SELECT diff_type, from_row FROM dolt_diff('HEAD~', 'HEAD', 'bag')
ORDER BY from_row->>'i';

-- a name neither version has is refused; the row-type form, unchanged
SELECT * FROM dolt_diff('HEAD~', 'HEAD', 'no_such_table');
SELECT * FROM dolt_diff('HEAD~', 'HEAD', NULL::int);
SELECT * FROM dolt_diff('HEAD~', 'HEAD', NULL::text);

-- a version whose column's type this database hasn't now is refused
CREATE TYPE dn.mood AS ENUM ('ok', 'sad');
CREATE TABLE dn.feel (id int PRIMARY KEY, m dn.mood);
INSERT INTO dn.feel VALUES (1, 'ok');
SELECT length(dolt_commit('-Am', 'feel')::text) AS committed;
DROP TABLE dn.feel;
DROP TYPE dn.mood;
SELECT length(dolt_commit('-am', 'feel dropped')::text) AS committed;
SELECT * FROM dolt_diff('HEAD~', 'HEAD', 'feel');

-- the table of a version's name the session has: SELECT on it; a dropped
-- one, no grant to check: the database's owner, or pg_read_all_data
-- (a plan's row estimate for a query)
CREATE FUNCTION pg_temp.estimated(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	line text;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN ' || q LOOP
		RETURN substring(line from 'rows=[0-9]+');
	END LOOP;
END $$;
CREATE ROLE regress_dn_reader;
GRANT USAGE ON SCHEMA dn TO regress_dn_reader;
SET ROLE regress_dn_reader;
SELECT count(*) FROM dolt_diff('HEAD~5', 'HEAD~4', 'r3');
SELECT count(*) FROM dolt_diff('HEAD~3', 'HEAD~2', 'bag');
RESET ROLE;
GRANT SELECT ON dn.r3 TO regress_dn_reader;
SET ROLE regress_dn_reader;
SELECT count(*) FROM dolt_diff('HEAD~5', 'HEAD~4', 'r3');
SELECT count(*) FROM dolt_diff('HEAD~3', 'HEAD~2', 'bag');
RESET ROLE;
GRANT pg_read_all_data TO regress_dn_reader;
SET ROLE regress_dn_reader;
SELECT count(*) FROM dolt_diff('HEAD~3', 'HEAD~2', 'bag');
RESET ROLE;
REVOKE pg_read_all_data FROM regress_dn_reader;

-- a table made with a dropped one's name isn't it: SELECT on it doesn't
-- cover the dropped one's rows, by any of the three, only its own
CREATE TABLE dn.secret (id int PRIMARY KEY, s text);
INSERT INTO dn.secret VALUES (1, 'top secret');
SELECT length(dolt_commit('-Am', 'secret')::text) AS committed;
DROP TABLE dn.secret;
SELECT length(dolt_commit('-am', 'secret dropped')::text) AS committed;
CREATE TABLE dn.secret (id int PRIMARY KEY, note text);
INSERT INTO dn.secret VALUES (2, 'public');
GRANT SELECT ON dn.secret TO regress_dn_reader;
SELECT length(dolt_commit('-Am', 'public secret')::text) AS committed;
SET ROLE regress_dn_reader;
SELECT * FROM dolt_diff('HEAD~2', 'HEAD~', 'dn.secret');
SELECT * FROM dolt_diff('HEAD~2', 'HEAD', 'dn.secret');
SELECT diff_type, from_row FROM dolt_diff_table('dn.secret');
SELECT row FROM dolt_history_table('dn.secret');
SELECT diff_type, to_row FROM dolt_diff('HEAD~', 'HEAD', 'dn.secret');
SELECT diff_type, to_row FROM dolt_diff_table('dn.secret', 'HEAD~');
-- the same versions read by the table's row type: the same rule, so the
-- dropped table's rows aren't read on the grants of the one made again
SELECT * FROM dolt_at('HEAD~2', NULL::dn.secret);
SELECT * FROM dolt_history_table(NULL::dn.secret);
SELECT * FROM dolt_diff('HEAD~2', 'HEAD', NULL::dn.secret);
SELECT diff_type FROM dolt_diff_table(NULL::dn.secret);
SELECT * FROM dolt_at('HEAD', NULL::dn.secret);
SELECT diff_type, to_row FROM dolt_diff('HEAD~', 'HEAD', NULL::dn.secret);
SELECT diff_type, to_row FROM dolt_diff_table(NULL::dn.secret, 'HEAD~');
-- a planner's estimate of the version's rows is history too: refused, the
-- default estimate taken
SELECT pg_temp.estimated('SELECT * FROM dolt_at(''HEAD~2'', NULL::dn.secret)');
SELECT pg_temp.estimated('SELECT * FROM dolt_at(''HEAD'', NULL::dn.secret)');
-- and a statistic of its rows: left out, and refused by name
SELECT table_name, rows_deleted FROM dolt_diff_stat('HEAD~2', 'HEAD~');
SELECT table_name, rows_deleted FROM dolt_diff_stat('HEAD~2', 'HEAD~', 'dn.secret');
SELECT table_name, rows_added FROM dolt_diff_stat('HEAD~', 'HEAD');
RESET ROLE;
-- the head moved back to the dropped table's commit: not by the reader,
-- whose SELECT moves no head; by the owner, after which the head's version
-- is no longer this table's by the branch's record of its head, so the
-- step from the working set to the head vouches for nothing
SELECT dolt_hashof('HEAD') AS secret_head \gset
SET ROLE regress_dn_reader;
SELECT dolt_reset('--soft', 'HEAD~2') IS NOT NULL AS moved;
RESET ROLE;
SELECT dolt_reset('--soft', 'HEAD~2') IS NOT NULL AS moved;
SET ROLE regress_dn_reader;
SELECT * FROM dolt_at('HEAD', NULL::dn.secret);
SELECT row FROM dolt_history_table(NULL::dn.secret);
SELECT row FROM dolt_history_table('dn.secret');
SELECT * FROM dolt_at('WORKING', NULL::dn.secret);
RESET ROLE;
SELECT dolt_reset('--soft', :'secret_head') IS NOT NULL AS back;
SET ROLE regress_dn_reader;
SELECT * FROM dolt_at('HEAD', NULL::dn.secret);
RESET ROLE;
-- (the database's owner reads both)
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~2', 'HEAD', 'dn.secret');
SELECT * FROM dolt_at('HEAD~2', NULL::dn.secret);

-- another branch's commits: its version of this table (made from this
-- table's history) is readable on its grants, as Dolt reads any commit;
-- another table of its name, made on that branch, isn't
SELECT dolt_branch('dn_side') IS NOT NULL AS made;
SELECT dolt_branch('dn_other') IS NOT NULL AS made;
SELECT dolt_checkout('dn_side') IS NOT NULL AS on_side;
INSERT INTO dn.secret VALUES (3, 'side');
SELECT length(dolt_commit('-am', 'side row')::text) AS committed;
SELECT dolt_checkout('dn_other') IS NOT NULL AS on_other;
DROP TABLE dn.secret;
SELECT length(dolt_commit('-am', 'no secret')::text) AS committed;
CREATE TABLE dn.secret (id int PRIMARY KEY, note text);
INSERT INTO dn.secret VALUES (4, 'other secret');
SELECT length(dolt_commit('-Am', 'another secret')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SET ROLE regress_dn_reader;
SELECT * FROM dolt_at('dn_side', NULL::dn.secret) ORDER BY id;
SELECT diff_type, to_row FROM dolt_diff('HEAD', 'dn_side', NULL::dn.secret);
SELECT * FROM dolt_at('dn_other', NULL::dn.secret);
SELECT * FROM dolt_diff('HEAD', 'dn_other', 'dn.secret');
RESET ROLE;
-- the head moved without a commit of the branch's (its record of its
-- tables is of the commit before): a fast-forward to a later version of
-- the table, and a soft reset to an earlier one, keep the reader reading
-- the table's own history
SELECT (dolt_merge('dn_side'))[2] AS fast_forward;
SET ROLE regress_dn_reader;
SELECT * FROM dolt_at('HEAD', NULL::dn.secret) ORDER BY id;
SELECT * FROM dolt_at('HEAD~', NULL::dn.secret) ORDER BY id;
RESET ROLE;
INSERT INTO dn.secret VALUES (5, 'five');
SELECT length(dolt_commit('-am', 'row 5')::text) AS committed;
SELECT dolt_hashof('HEAD') AS five_head \gset
SELECT dolt_reset('--soft', 'HEAD~') IS NOT NULL AS moved;
SET ROLE regress_dn_reader;
SELECT * FROM dolt_at('HEAD', NULL::dn.secret) ORDER BY id;
RESET ROLE;
SELECT dolt_reset('--soft', :'five_head') IS NOT NULL AS back;
-- (a head moved to the dropped table's commit and then committed records
-- the re-made table at a commit whose parent holds the dropped one, which
-- the names and identities alone can't tell from one table: #891, #895)
SELECT dolt_branch('-D', 'dn_side') IS NOT NULL AS deleted;
SELECT dolt_branch('-D', 'dn_other') IS NOT NULL AS deleted;

-- nor one made with a renamed one's old name: the renamed one's rows by
-- that name, without SELECT on it
CREATE TABLE dn.k1 (id int PRIMARY KEY, s text);
INSERT INTO dn.k1 VALUES (1, 'kept');
SELECT length(dolt_commit('-Am', 'k1')::text) AS committed;
ALTER TABLE dn.k1 RENAME TO k2;
SELECT length(dolt_commit('-am', 'k1 renamed')::text) AS committed;
CREATE TABLE dn.k1 (id int PRIMARY KEY);
GRANT SELECT ON dn.k1 TO regress_dn_reader;
SELECT length(dolt_commit('-Am', 'k1 again')::text) AS committed;
SET ROLE regress_dn_reader;
SELECT * FROM dolt_diff('HEAD~3', 'HEAD~2', 'k1');
SELECT count(*) FROM dolt_history_table('k1');
RESET ROLE;
GRANT SELECT ON dn.k2 TO regress_dn_reader;
SET ROLE regress_dn_reader;
-- (k2's grant covers its version by the name it had, across the rename)
SELECT count(*) FROM dolt_diff('HEAD~2', 'HEAD~', 'k2');
SELECT count(*) FROM dolt_diff_table('k2');
RESET ROLE;

-- a diff of two commits by name beside uncommitted changes: the
-- database's owner reads no working-set row (no lineage to walk); a
-- reader with a grant gets the same rows
INSERT INTO dn.k2 VALUES (2, 'two');
SELECT length(dolt_commit('-am', 'k2 two')::text) AS committed;
INSERT INTO dn.k2 SELECT g, 'dirty' FROM generate_series(10, 509) g;
SELECT count(*) AS reset FROM test_fold_counters(true);
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~', 'HEAD', 'k2');
SELECT pages, candidates, full_tables FROM test_fold_counters(true);
SET ROLE regress_dn_reader;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~', 'HEAD', 'k2');
RESET ROLE;
DELETE FROM dn.k2 WHERE id >= 10;
DROP OWNED BY regress_dn_reader;
DROP ROLE regress_dn_reader;

\c contrib_regression
DROP DATABASE regress_diff_by_name;
