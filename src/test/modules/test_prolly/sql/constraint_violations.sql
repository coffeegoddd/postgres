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

-- A row a merge updates that breaks a unique constraint is recorded too.
SELECT dolt_branch('-D', 'feature');
SELECT dolt_branch('feature');
SELECT dolt_checkout('feature');
UPDATE cv.child SET code = 'z' WHERE id = 1;
SELECT length(dolt_commit('-am', 'feature recodes 1')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO cv.child VALUES (7, 1, 'z');
SELECT length(dolt_commit('-am', 'main takes z')::text) AS committed;
BEGIN;
SELECT (dolt_merge('feature'))[3:4];
SELECT violation_type, violating_row FROM dolt_constraint_violations('cv.child');
-- (a table dropped since has none to fix)
SAVEPOINT s;
DROP TABLE cv.child;
SELECT * FROM dolt_constraint_violations();
ROLLBACK TO s;
SELECT count(*) AS still_listed FROM dolt_constraint_violations();
ROLLBACK;

-- Violations kept stay through later merges that find none of their own.
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('feature'))[3:4];
SELECT length(dolt_commit('-am', 'merged, a violation kept')::text) AS committed;
RESET dolt.allow_commit_conflicts;
SELECT dolt_branch('other');
SELECT dolt_checkout('other');
INSERT INTO cv.parent VALUES (3);
SELECT length(dolt_commit('-am', 'other adds a parent')::text) AS committed;
SELECT dolt_checkout('main');
SET dolt.allow_commit_conflicts = on;
INSERT INTO cv.parent VALUES (4);
SELECT length(dolt_commit('-am', 'main adds a parent')::text) AS committed;
SELECT (dolt_merge('other', '-m', 'merged other'))[3:4];
RESET dolt.allow_commit_conflicts;
SELECT table_name, num_violations FROM dolt_constraint_violations();

-- A branch deleted takes its violations with it.
SELECT dolt_checkout('other');
SET session_replication_role = replica;	-- (no foreign key checks)
INSERT INTO cv.child VALUES (8, 98, 'orphan on other');
RESET session_replication_role;
SET dolt.allow_commit_conflicts = on;
SELECT dolt_verify_constraints('cv.child');
RESET dolt.allow_commit_conflicts;
SELECT count(*) AS on_other FROM dolt_constraint_violations();
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'other');
SELECT dolt_branch('other');
SELECT dolt_checkout('other');
SELECT count(*) AS on_a_new_other FROM dolt_constraint_violations();
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'other');
SET dolt.allow_commit_conflicts = on;
SELECT dolt_verify_constraints();
RESET dolt.allow_commit_conflicts;
DELETE FROM cv.child WHERE id = 7;
SELECT dolt_verify_constraints();
SELECT length(dolt_commit('-am', 'fixed')::text) AS committed;

-- A revert that leaves a child without its parent records it.
INSERT INTO cv.parent VALUES (10);
SELECT (dolt_commit('-am', 'parent 10'))[1] AS parent10 \gset
INSERT INTO cv.child VALUES (10, 10, 'ten');
SELECT length(dolt_commit('-am', 'child of 10')::text) AS committed;
SELECT dolt_revert(:'parent10');
BEGIN;
SELECT dolt_revert(:'parent10');
SELECT violation_type, violating_row FROM dolt_constraint_violations('cv.child');
-- dolt_commit refuses them, unless forced; the transaction still can't
-- commit with them.
SAVEPOINT s;
SELECT dolt_commit('-am', 'not with violations');
ROLLBACK TO s;
SELECT length(dolt_commit('-afm', 'forced')::text) AS committed;
SELECT length(dolt_commit('--force', '--allow-empty', '-m', 'forced again')::text) AS committed;
COMMIT;
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;

-- Rows a parent's row security hides from its owner are seen anyway, as
-- the foreign key's own checks see them.
CREATE ROLE regress_cv_owner;
GRANT USAGE ON SCHEMA cv TO regress_cv_owner;
ALTER TABLE cv.parent OWNER TO regress_cv_owner;
ALTER TABLE cv.child OWNER TO regress_cv_owner;
ALTER TABLE cv.parent ENABLE ROW LEVEL SECURITY;
ALTER TABLE cv.parent FORCE ROW LEVEL SECURITY;
CREATE POLICY hide ON cv.parent USING (false);
SET ROLE regress_cv_owner;
SELECT count(*) AS hidden FROM cv.parent;
SELECT dolt_verify_constraints('cv.child');
SELECT count(*) AS none FROM dolt_constraint_violations();
RESET ROLE;

-- A keyed row whose new version breaks a unique index stays as the branch
-- has it, whether the merge applies theirs' changes alone (u: ours changed
-- its definition only) or merges both sides' (w); and so does one whose
-- new version would take the value a row kept so still holds (w's 1).
CREATE SCHEMA uv VERSIONED;
CREATE TABLE uv.u (pk int PRIMARY KEY, x int);
INSERT INTO uv.u VALUES (1, 1), (2, 2);
CREATE TABLE uv.w (pk int PRIMARY KEY, x int UNIQUE);
INSERT INTO uv.w VALUES (1, 1), (2, 2);
SELECT length(dolt_commit('-Am', 'uv base')::text) AS committed;
SELECT dolt_branch('uv_theirs');
CREATE UNIQUE INDEX u_x ON uv.u (x);
INSERT INTO uv.w VALUES (3, 5);
SELECT length(dolt_commit('-Am', 'uv ours')::text) AS committed;
SELECT dolt_checkout('uv_theirs');
UPDATE uv.u SET x = 1 WHERE pk = 2;
UPDATE uv.w SET x = 5 WHERE pk = 2;
UPDATE uv.w SET x = 2 WHERE pk = 1;
SELECT length(dolt_commit('-am', 'uv theirs')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('uv_theirs'))[3:4];
SELECT * FROM uv.u ORDER BY pk;
SELECT * FROM uv.w ORDER BY pk;
SELECT 'u' AS t, violation_type, violating_row::text
FROM dolt_constraint_violations('uv.u')
UNION ALL
SELECT 'w', violation_type, violating_row::text
FROM dolt_constraint_violations('uv.w') ORDER BY 1, 3;
ROLLBACK;
SELECT dolt_branch('-D', 'uv_theirs');
DROP SCHEMA uv CASCADE;
SELECT length(dolt_commit('-Am', 'uv dropped')::text) AS committed;

SELECT dolt_branch('-D', 'feature');
DROP SCHEMA cv CASCADE;
DROP ROLE regress_cv_owner;
