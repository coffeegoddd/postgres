-- Rows both sides of a merge changed merge column by column, as in Dolt:
-- a column one side changed takes that side's value, one both changed
-- alike keeps it, and only a column they changed differently makes the
-- row a conflict.

CREATE SCHEMA cm VERSIONED;
CREATE TABLE cm.t (pk int PRIMARY KEY, a int, b text, c int);
INSERT INTO cm.t VALUES (1, 1, 'one', 1), (2, 2, 'two', 2), (3, 3, 'three', 3),
	(4, 4, 'four', 4), (5, 5, 'five', 5), (6, 6, 'six', 6);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('cm_theirs');
SELECT dolt_checkout('cm_theirs');
UPDATE cm.t SET a = 10 WHERE pk = 1;			-- ours changes b
UPDATE cm.t SET a = 20, b = 'TWO' WHERE pk = 2;	-- ours changes b alike, and c
UPDATE cm.t SET a = 30 WHERE pk = 3;			-- ours changes a differently
UPDATE cm.t SET b = NULL WHERE pk = 4;			-- ours changes c
DELETE FROM cm.t WHERE pk = 5;					-- ours changes a
UPDATE cm.t SET c = 60 WHERE pk = 6;			-- ours deletes it
SELECT length(dolt_commit('-am', 'theirs')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE cm.t SET b = 'ONE' WHERE pk = 1;
UPDATE cm.t SET b = 'TWO', c = 22 WHERE pk = 2;
UPDATE cm.t SET a = 33 WHERE pk = 3;
UPDATE cm.t SET c = 44 WHERE pk = 4;
UPDATE cm.t SET a = 55 WHERE pk = 5;
DELETE FROM cm.t WHERE pk = 6;
SELECT length(dolt_commit('-am', 'ours')::text) AS committed;

BEGIN;
SELECT (dolt_merge('cm_theirs'))[2:4];
SELECT * FROM cm.t ORDER BY pk;
-- the rows changed differently, and a deletion against a change
SELECT base_row, our_row, their_row, our_diff_type, their_diff_type
FROM dolt_conflicts(NULL::cm.t) ORDER BY (base_row).pk;
-- resolved with theirs: those rows alone become theirs
SELECT dolt_conflicts_resolve('--theirs', 'cm.t');
SELECT * FROM cm.t ORDER BY pk;
SELECT length(dolt_commit('-m', 'merged')::text) AS committed;
COMMIT;

-- Both directions merge the same columns.
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_checkout('cm_theirs');
BEGIN;
SELECT (dolt_merge('main'))[2:4];
SELECT * FROM cm.t WHERE pk IN (1, 2, 4) ORDER BY pk;
SELECT (base_row).pk FROM dolt_conflicts(NULL::cm.t) ORDER BY 1;
ROLLBACK;
SELECT dolt_checkout('main');

-- A merged row is written as one: one breaking a unique constraint is a
-- violation, the row left as the branch had it.
CREATE TABLE cm.u (pk int PRIMARY KEY, col1 int, col2 int, UNIQUE (col1, col2));
INSERT INTO cm.u VALUES (1, 1, 1), (2, NULL, NULL);
SELECT length(dolt_commit('-Am', 'unique')::text) AS committed;
SELECT dolt_branch('cm_right');
UPDATE cm.u SET col1 = 1 WHERE pk = 2;
SELECT length(dolt_commit('-am', 'left')::text) AS committed;
SELECT dolt_checkout('cm_right');
UPDATE cm.u SET col2 = 1 WHERE pk = 2;
SELECT length(dolt_commit('-am', 'right')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('cm_right'))[2:4];
SELECT * FROM cm.u ORDER BY pk;
SELECT violation_type, violating_row FROM dolt_constraint_violations('cm.u');
ROLLBACK;

-- Columns both set to NULL agree; one set to NULL and one changed don't.
CREATE TABLE cm.n (pk int PRIMARY KEY, v int, w int);
INSERT INTO cm.n VALUES (1, 1, 1), (2, 2, 2);
SELECT length(dolt_commit('-Am', 'nulls')::text) AS committed;
SELECT dolt_branch('cm_nulls');
UPDATE cm.n SET v = NULL;
SELECT length(dolt_commit('-am', 'ours nulls')::text) AS committed;
SELECT dolt_checkout('cm_nulls');
UPDATE cm.n SET v = NULL, w = 10 WHERE pk = 1;
UPDATE cm.n SET v = 20 WHERE pk = 2;
SELECT length(dolt_commit('-am', 'theirs nulls')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('cm_nulls'))[2:4];
SELECT * FROM cm.n ORDER BY pk;
SELECT (base_row).pk FROM dolt_conflicts(NULL::cm.n);
ROLLBACK;

-- A stored generated column isn't compared: it follows the columns
-- merged, each side's change to its own.
CREATE TABLE cm.g (pk int PRIMARY KEY, a int, b int,
	g int GENERATED ALWAYS AS (a + b) STORED);
INSERT INTO cm.g VALUES (1, 1, 1);
SELECT length(dolt_commit('-Am', 'generated')::text) AS committed;
SELECT dolt_branch('cm_gen');
UPDATE cm.g SET a = 2;
SELECT length(dolt_commit('-am', 'ours a')::text) AS committed;
SELECT dolt_checkout('cm_gen');
UPDATE cm.g SET b = 5;
SELECT length(dolt_commit('-am', 'theirs b')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('cm_gen'))[2:4];
SELECT * FROM cm.g;
ROLLBACK;

-- Violations of a table dropped since: not listed (nor its status).
BEGIN;
SET LOCAL dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('cm_right'))[2:4];
SELECT * FROM dolt_constraint_violations();
DROP TABLE cm.u;
SELECT * FROM dolt_constraint_violations();
SELECT table_name, status FROM dolt_status WHERE table_name LIKE 'cm.%';
ROLLBACK;

SELECT dolt_branch('-D', 'cm_theirs');
SELECT dolt_branch('-D', 'cm_right');
SELECT dolt_branch('-D', 'cm_nulls');
SELECT dolt_branch('-D', 'cm_gen');
DROP SCHEMA cm CASCADE;
