-- Constraint violations a merge leaves, as in Dolt: rows it would write
-- that break a unique constraint aren't written, and rows of tables it
-- wrote that break a foreign key (which it doesn't check as it writes)
-- stay; both are recorded, listed in dolt_constraint_violations(), and
-- keep the merge from being committed until dolt_verify_constraints()
-- finds none left.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA cv VERSIONED;
CREATE TABLE cv.parent (id int PRIMARY KEY);
CREATE TABLE cv.child (id int PRIMARY KEY, pid int REFERENCES cv.parent,
	code text UNIQUE);
INSERT INTO cv.parent VALUES (1), (2);
INSERT INTO cv.child VALUES (1, 1, 'a');
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('feature');
SELECT dolt_checkout('feature');
INSERT INTO cv.child VALUES (2, 2, 'b');	-- a child of the parent main drops
INSERT INTO cv.child VALUES (4, 1, 'x');	-- a code main gives another row
SELECT length(dolt_commit('-am', 'feature children')::text) AS committed;
SELECT dolt_checkout('main');
DELETE FROM cv.parent WHERE id = 2;
INSERT INTO cv.child VALUES (3, 1, 'x');
SELECT length(dolt_commit('-am', 'main changes')::text) AS committed;

-- Outside a transaction block, the merge is rolled back.
SELECT dolt_merge('feature');
SELECT id FROM cv.child ORDER BY id;

-- In one, it stops with them recorded.
BEGIN;
SELECT (dolt_merge('feature'))[3:4];
SELECT * FROM dolt_constraint_violations();
SELECT violation_type, violating_row, violation_info
FROM dolt_constraint_violations('cv.child') ORDER BY violation_type;
SELECT id, pid, code FROM cv.child ORDER BY id;
SAVEPOINT s;
SELECT dolt_commit('-am', 'too soon');
ROLLBACK TO s;
-- fixed, and checked again
INSERT INTO cv.parent VALUES (2);
INSERT INTO cv.child VALUES (5, 1, 'x again');
SELECT dolt_verify_constraints('cv.child');
SELECT count(*) AS left_over FROM dolt_constraint_violations();
SELECT length(dolt_commit('-am', 'merged')::text) AS committed;
COMMIT;
SELECT dolt_hashof('HEAD^2') = dolt_hashof('feature') AS merge_commit;
SELECT id, pid, code FROM cv.child ORDER BY id;

-- dolt_verify_constraints() finds what other changes break, and a reset
-- clears what was recorded.
SET session_replication_role = replica;	-- (no foreign key checks)
INSERT INTO cv.child VALUES (6, 99, 'orphan');
RESET session_replication_role;
BEGIN;
SELECT dolt_verify_constraints();
SELECT violation_type, violating_row FROM dolt_constraint_violations('cv.child');
ROLLBACK;
SELECT dolt_verify_constraints();	-- can't be committed
SET dolt.allow_commit_conflicts = on;
SELECT dolt_verify_constraints();	-- kept
RESET dolt.allow_commit_conflicts;
SELECT count(*) FROM dolt_constraint_violations();
SELECT dolt_reset('--hard');
SELECT count(*) FROM dolt_constraint_violations();

SELECT dolt_branch('-D', 'feature');
DROP SCHEMA cv CASCADE;
