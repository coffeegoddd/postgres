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
SELECT dolt_branch('no_child');
SELECT dolt_checkout('no_child');
DROP TABLE cv.child;
SELECT length(dolt_commit('-am', 'no_child drops cv.child')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('feature'))[3:4];
SELECT violation_type, violating_row FROM dolt_constraint_violations('cv.child');
-- (a table dropped since has none to fix)
SAVEPOINT s;
DROP TABLE cv.child;
SELECT * FROM dolt_constraint_violations();
ROLLBACK TO s;
SELECT count(*) AS still_listed FROM dolt_constraint_violations();
-- (and a checkout to a branch without the table doesn't let them through)
SELECT dolt_checkout('no_child');
COMMIT;
SELECT dolt_checkout('main');
SELECT count(*) AS none_kept FROM dolt_constraint_violations();
SELECT dolt_branch('-D', 'no_child');

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

-- A deferrable unique or exclusion constraint is checked as the merge
-- writes each row, as one that isn't deferrable is: a row breaking it is
-- recorded, not left for the end of the merge or the commit to refuse.
-- So is a deferrable primary key, or one WITHOUT OVERLAPS (neither keys
-- its table's rows, so the merge doesn't decide rows by it).  A row breaking
-- an exclusion constraint (one WITHOUT OVERLAPS's too) is recorded as an
-- "exclusion constraint", not a "unique index", naming the constraint.
CREATE SCHEMA dv VERSIONED;
CREATE TABLE dv.later (pk int PRIMARY KEY,
	x int UNIQUE DEFERRABLE INITIALLY DEFERRED);
CREATE TABLE dv.now (pk int PRIMARY KEY, x int UNIQUE DEFERRABLE);
CREATE TABLE dv.pk (pk int PRIMARY KEY DEFERRABLE, x int);
CREATE TABLE dv.ex (pk int PRIMARY KEY, r int4range,
	EXCLUDE USING gist (r WITH &&) DEFERRABLE INITIALLY DEFERRED);
CREATE TABLE dv.wo (k int4range, during int4range,
	PRIMARY KEY (k, during WITHOUT OVERLAPS));
INSERT INTO dv.later VALUES (1, 1);
INSERT INTO dv.now VALUES (1, 1);
INSERT INTO dv.pk VALUES (1, 1);
INSERT INTO dv.ex VALUES (1, '[1,2)');
INSERT INTO dv.wo VALUES ('[1,2)', '[1,3)');
SELECT length(dolt_commit('-Am', 'deferrable base')::text) AS committed;
SELECT dolt_branch('deferring');
SELECT dolt_checkout('deferring');
INSERT INTO dv.later VALUES (3, 5), (4, NULL);
INSERT INTO dv.now VALUES (3, 5);
INSERT INTO dv.pk VALUES (2, 3);
INSERT INTO dv.ex VALUES (3, '[5,7)');
INSERT INTO dv.wo VALUES ('[1,2)', '[5,8)'), ('[2,3)', '[5,8)');
SELECT length(dolt_commit('-am', 'theirs')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO dv.later VALUES (2, 5), (5, NULL);
INSERT INTO dv.now VALUES (2, 5);
INSERT INTO dv.pk VALUES (2, 2);
INSERT INTO dv.ex VALUES (2, '[6,8)');
INSERT INTO dv.wo VALUES ('[1,2)', '[6,9)');
SELECT length(dolt_commit('-am', 'ours')::text) AS committed;
BEGIN;
SELECT (dolt_merge('deferring'))[3:4];
SELECT table_name, violation_type, violating_row, violation_info
FROM dolt_constraint_violations() d,
	LATERAL dolt_constraint_violations(d.table_name::regclass)
ORDER BY table_name;
SELECT * FROM dv.later ORDER BY pk;
SELECT * FROM dv.now ORDER BY pk;
SELECT * FROM dv.pk ORDER BY pk, x;
SELECT * FROM dv.ex ORDER BY pk;
SELECT * FROM dv.wo ORDER BY k, during;
SELECT dolt_verify_constraints();
SELECT length(dolt_commit('-am', 'merged deferring')::text) AS committed;
COMMIT;
SELECT dolt_branch('-D', 'deferring');
DROP SCHEMA dv CASCADE;
SELECT length(dolt_commit('-Am', 'no dv')::text) AS committed;

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
-- A row whose put-back was refused keeps the branch's version while the
-- others are written, then takes its new one if it fits: r's 2 and s's 2
-- (s: theirs' changes alone, as u) do once 1 is refused, so only 1's
-- violation is recorded.  Such a row is written first the next time, so a
-- row refused only for the values it holds isn't (a's 1, c's 1), and two
-- rows swapping a value can still both be written (b's 1 and 3).  A row
-- whose put-back is refused again is left in place, then updated (p: no
-- row of theirs fits, as each takes a value the other keeps).  A known
-- limit (x): rows refused are tried again one at a time, so two that
-- swap a value (3 and 4 take each other's b) stay refused, though they
-- would fit together: no row of theirs is written, and four violations
-- are recorded where two would do.  Each is a real one, and the table
-- holds no row breaking a constraint.
CREATE SCHEMA uv VERSIONED;
CREATE TABLE uv.u (pk int PRIMARY KEY, x int);
INSERT INTO uv.u VALUES (1, 1), (2, 2);
CREATE TABLE uv.w (pk int PRIMARY KEY, x int UNIQUE);
INSERT INTO uv.w VALUES (1, 1), (2, 2);
CREATE TABLE uv.r (pk int PRIMARY KEY, b int UNIQUE, c int);
INSERT INTO uv.r VALUES (1, 100, 0), (2, 200, 0);
CREATE TABLE uv.s (pk int PRIMARY KEY, b int, c int);
INSERT INTO uv.s VALUES (1, 100, 0), (2, 999, 0);
CREATE TABLE uv.a (pk int PRIMARY KEY, b int UNIQUE, c int UNIQUE);
INSERT INTO uv.a VALUES (1, 10, 100), (2, 20, 200), (3, 30, 300);
CREATE TABLE uv.b (pk int PRIMARY KEY, b int UNIQUE, c int UNIQUE);
INSERT INTO uv.b VALUES (1, 10, 100), (2, 20, 200), (3, 30, 300);
CREATE TABLE uv.c (pk int PRIMARY KEY, b int, c int);
INSERT INTO uv.c VALUES (1, 10, 100), (2, 20, 200), (3, 99, 300);
CREATE TABLE uv.p (pk int PRIMARY KEY, b int, c int);
INSERT INTO uv.p VALUES (1, 4, 1), (2, 3, 2), (3, 2, 4);
CREATE TABLE uv.x (pk int PRIMARY KEY, b int, c int);
INSERT INTO uv.x VALUES (1, 2, 3), (2, 4, 0), (3, 3, 1), (4, 0, 2);
SELECT length(dolt_commit('-Am', 'uv base')::text) AS committed;
SELECT dolt_branch('uv_theirs');
CREATE UNIQUE INDEX u_x ON uv.u (x);
INSERT INTO uv.w VALUES (3, 5);
UPDATE uv.r SET b = 999 WHERE pk = 2;
CREATE UNIQUE INDEX s_b ON uv.s (b);
UPDATE uv.a SET b = 99 WHERE pk = 3;
UPDATE uv.b SET c = 999 WHERE pk = 3;
CREATE UNIQUE INDEX c_b ON uv.c (b);
CREATE UNIQUE INDEX c_c ON uv.c (c);
CREATE UNIQUE INDEX p_b ON uv.p (b);
CREATE UNIQUE INDEX p_c ON uv.p (c);
CREATE UNIQUE INDEX x_b ON uv.x (b);
CREATE UNIQUE INDEX x_c ON uv.x (c);
SELECT length(dolt_commit('-Am', 'uv ours')::text) AS committed;
SELECT dolt_checkout('uv_theirs');
UPDATE uv.u SET x = 1 WHERE pk = 2;
UPDATE uv.w SET x = 5 WHERE pk = 2;
UPDATE uv.w SET x = 2 WHERE pk = 1;
UPDATE uv.r SET b = 999 WHERE pk = 1;
UPDATE uv.r SET c = 1 WHERE pk = 2;
UPDATE uv.s SET b = 999 WHERE pk = 1;
UPDATE uv.s SET c = 1 WHERE pk = 2;
UPDATE uv.a SET b = 99 WHERE pk = 2;
UPDATE uv.a SET c = 301 WHERE pk = 3;
UPDATE uv.a SET c = 300 WHERE pk = 1;
UPDATE uv.b SET c = 999 WHERE pk = 2;
UPDATE uv.b SET b = 0 WHERE pk = 1;
UPDATE uv.b SET b = 10 WHERE pk = 3;
UPDATE uv.b SET b = 30 WHERE pk = 1;
UPDATE uv.c SET b = 99 WHERE pk = 2;
UPDATE uv.c SET c = 301 WHERE pk = 3;
UPDATE uv.c SET c = 300 WHERE pk = 1;
UPDATE uv.p SET c = 2 WHERE pk = 1;
UPDATE uv.p SET b = 4 WHERE pk = 2;
UPDATE uv.x SET b = (ARRAY[4, 3, 0, 3])[pk];
SELECT length(dolt_commit('-am', 'uv theirs')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('uv_theirs'))[3:4];
SELECT * FROM uv.u ORDER BY pk;
SELECT * FROM uv.w ORDER BY pk;
SELECT * FROM uv.r ORDER BY pk;
SELECT * FROM uv.s ORDER BY pk;
SELECT * FROM uv.a ORDER BY pk;
SELECT * FROM uv.b ORDER BY pk;
SELECT * FROM uv.c ORDER BY pk;
SELECT * FROM uv.p ORDER BY pk;
SELECT * FROM uv.x ORDER BY pk;
SELECT split_part(t, '.', 2) AS t, violation_type, violating_row::text
FROM unnest(ARRAY['uv.u', 'uv.w', 'uv.r', 'uv.s', 'uv.a', 'uv.b', 'uv.c',
				  'uv.p', 'uv.x']) t,
	LATERAL dolt_constraint_violations(t)
ORDER BY 1, 3;
ROLLBACK;
SELECT dolt_branch('-D', 'uv_theirs');
DROP SCHEMA uv CASCADE;
SELECT length(dolt_commit('-Am', 'uv dropped')::text) AS committed;

-- The rows put back and the pinned row updated are checked against a
-- deferrable unique constraint as each is written too, so a deferrable
-- one (d, ad) merges as one that isn't (i, ai).  A row whose new version
-- takes a value a row of ours holds (d's 2) stays as the branch has it, as
-- does the row that would take its old value (d's 1); and a pinned row's
-- update (ad's 3, as uv.a's) neither leaves a buffer pinned nor fails.
CREATE SCHEMA dw VERSIONED;
CREATE TABLE dw.d (pk int PRIMARY KEY, x int UNIQUE DEFERRABLE);
CREATE TABLE dw.i (pk int PRIMARY KEY, x int UNIQUE);
CREATE TABLE dw.ad (pk int PRIMARY KEY, b int UNIQUE DEFERRABLE,
	c int UNIQUE DEFERRABLE INITIALLY DEFERRED);
CREATE TABLE dw.ai (pk int PRIMARY KEY, b int UNIQUE, c int UNIQUE);
INSERT INTO dw.d VALUES (1, 4), (2, 5);
INSERT INTO dw.i VALUES (1, 4), (2, 5);
INSERT INTO dw.ad VALUES (1, 10, 100), (2, 20, 200), (3, 30, 300);
INSERT INTO dw.ai VALUES (1, 10, 100), (2, 20, 200), (3, 30, 300);
SELECT length(dolt_commit('-Am', 'dw base')::text) AS committed;
SELECT dolt_branch('dw_theirs');
INSERT INTO dw.d VALUES (3, 7);
INSERT INTO dw.i VALUES (3, 7);
UPDATE dw.ad SET b = 99 WHERE pk = 3;
UPDATE dw.ai SET b = 99 WHERE pk = 3;
SELECT length(dolt_commit('-am', 'dw ours')::text) AS committed;
SELECT dolt_checkout('dw_theirs');
UPDATE dw.d SET x = 7 WHERE pk = 2;
UPDATE dw.d SET x = 5 WHERE pk = 1;
UPDATE dw.i SET x = 7 WHERE pk = 2;
UPDATE dw.i SET x = 5 WHERE pk = 1;
UPDATE dw.ad SET b = 99 WHERE pk = 2;
UPDATE dw.ad SET c = 301 WHERE pk = 3;
UPDATE dw.ad SET c = 300 WHERE pk = 1;
UPDATE dw.ai SET b = 99 WHERE pk = 2;
UPDATE dw.ai SET c = 301 WHERE pk = 3;
UPDATE dw.ai SET c = 300 WHERE pk = 1;
SELECT length(dolt_commit('-am', 'dw theirs')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('dw_theirs'))[3:4];
SELECT * FROM dw.d ORDER BY pk;
SELECT * FROM dw.i ORDER BY pk;
SELECT * FROM dw.ad ORDER BY pk;
SELECT * FROM dw.ai ORDER BY pk;
SELECT split_part(t, '.', 2) AS t, violation_type, violating_row::text
FROM unnest(ARRAY['dw.d', 'dw.i', 'dw.ad', 'dw.ai']) t,
	LATERAL dolt_constraint_violations(t)
ORDER BY 1, 3;
ROLLBACK;
SELECT dolt_branch('-D', 'dw_theirs');
DROP SCHEMA dw CASCADE;
SELECT length(dolt_commit('-Am', 'dw dropped')::text) AS committed;

-- Violations kept by one transaction keep any later one that writes the
-- branch from committing, as Dolt's, whoever found them; one that only
-- reads still commits.  Fixing the rows takes checking them again in the
-- same transaction: a fix alone still leaves them recorded.
SET session_replication_role = replica;	-- (no foreign key checks)
INSERT INTO cv.child VALUES (90, 97, 'orphan kept');
RESET session_replication_role;
SET dolt.allow_commit_conflicts = on;
SELECT dolt_verify_constraints('cv.child');
RESET dolt.allow_commit_conflicts;
SELECT count(*) AS kept FROM dolt_constraint_violations('cv.child');
SELECT count(*) AS read_only_commits FROM cv.child WHERE id = 90;
INSERT INTO cv.parent VALUES (50);
UPDATE cv.child SET pid = 1 WHERE id = 90;
BEGIN;
UPDATE cv.child SET pid = 1 WHERE id = 90;
SELECT dolt_verify_constraints('cv.child');
COMMIT;
SELECT count(*) AS left_over FROM dolt_constraint_violations();
SELECT pid FROM cv.child WHERE id = 90;
DELETE FROM cv.child WHERE id = 90;

-- A row refused for an exclusion constraint is tried again after the
-- others, as one refused for a unique index is: a later row may free its
-- value.  Theirs shifts c down the 101..105 run (and pk 101's onto pk 3's
-- old value), each row onto the value the next frees, so each, refused at
-- first, is written once the row holding its value has moved: none of
-- them is left a violation.  (pk 1's and 2's b collide with ours' unique
-- index for good.)
CREATE EXTENSION IF NOT EXISTS btree_gist;
CREATE SCHEMA ex VERSIONED;
CREATE TABLE ex.a (pk int PRIMARY KEY, b int, c int);
INSERT INTO ex.a VALUES (1, 10, 100), (2, 20, 200), (3, 30, 300);
INSERT INTO ex.a SELECT 100 + g, 1000 + g, 100000 + g FROM generate_series(1, 5) g;
SELECT length(dolt_commit('-Am', 'ex base')::text) AS committed;
SELECT dolt_branch('ex_side');
CREATE UNIQUE INDEX a_b ON ex.a (b);
ALTER TABLE ex.a ADD CONSTRAINT a_c EXCLUDE USING gist (c WITH =);
SELECT length(dolt_commit('-Am', 'ex ours')::text) AS committed;
SELECT dolt_checkout('ex_side');
UPDATE ex.a SET b = 20 WHERE pk = 1;
UPDATE ex.a SET b = 30, c = 999 WHERE pk = 2;
UPDATE ex.a SET b = 77, c = 999 WHERE pk = 3;
UPDATE ex.a SET c = CASE pk WHEN 101 THEN 300 ELSE 100000 + pk - 101 END
WHERE pk > 100;
SELECT length(dolt_commit('-Am', 'ex theirs')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('ex_side'))[3:4];
SELECT violating_row->>'pk' AS pk, violation_type
FROM dolt_constraint_violations('ex.a') ORDER BY 1;
SELECT * FROM ex.a ORDER BY pk;
ROLLBACK;
SELECT dolt_branch('-D', 'ex_side');
-- a commit of a transaction holding one is refused, naming it
CREATE TABLE ex.x (pk int PRIMARY KEY, c int, EXCLUDE USING gist (c WITH =));
SELECT length(dolt_commit('-Am', 'ex x')::text) AS committed;
SELECT dolt_branch('ex_x');
INSERT INTO ex.x VALUES (1, 5);
SELECT length(dolt_commit('-am', 'ex x ours')::text) AS committed;
SELECT dolt_checkout('ex_x');
INSERT INTO ex.x VALUES (2, 5);
SELECT length(dolt_commit('-am', 'ex x theirs')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('ex_x'))[3:4];
COMMIT;
SELECT dolt_branch('-D', 'ex_x');
DROP SCHEMA ex CASCADE;

-- A row a merge doesn't write for breaking unique indexes is recorded as
-- Dolt records it: once for each unique index it breaks, and so is each
-- row it collides with there (the table keeps them: a unique index holds
-- one of two).  An index of an expression, or with a predicate, has only
-- the refused row recorded, when its write was refused for it.
CREATE SCHEMA ux VERSIONED;
CREATE TABLE ux.t (pk int PRIMARY KEY, a int UNIQUE, b int UNIQUE, c int);
CREATE UNIQUE INDEX t_c_expr ON ux.t ((c * 2));
INSERT INTO ux.t VALUES (1, 1, 1, 1);
SELECT length(dolt_commit('-Am', 'ux base')::text) AS committed;
SELECT dolt_branch('ux_side');
INSERT INTO ux.t VALUES (2, 20, 30, 40), (3, 21, 31, 41);
SELECT length(dolt_commit('-am', 'ux ours')::text) AS committed;
SELECT dolt_checkout('ux_side');
-- (a row colliding with ours' pk 2 on a and pk 3 on b; one with pk 2 on c * 2)
INSERT INTO ux.t VALUES (4, 20, 31, 50), (5, 60, 60, 40);
SELECT length(dolt_commit('-am', 'ux theirs')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('ux_side'))[3:4];
SELECT violating_row->>'pk' AS pk, violation_info->>'Name' AS index
FROM dolt_constraint_violations('ux.t') ORDER BY 1, 2;
SELECT pk FROM ux.t ORDER BY pk;
ROLLBACK;
SELECT dolt_branch('-D', 'ux_side');
DROP SCHEMA ux CASCADE;

-- The rows collided with are as the merge left them: pk 1, whose a ours
-- moved to 9 and whose c theirs changed, is recorded with theirs' c
CREATE SCHEMA uy VERSIONED;
CREATE TABLE uy.t (pk int PRIMARY KEY, a int UNIQUE, c text);
INSERT INTO uy.t VALUES (1, 1, 'base');
SELECT length(dolt_commit('-Am', 'uy base')::text) AS committed;
SELECT dolt_branch('uy_side');
UPDATE uy.t SET a = 9;
SELECT length(dolt_commit('-am', 'uy ours')::text) AS committed;
SELECT dolt_checkout('uy_side');
UPDATE uy.t SET c = 'theirs';
INSERT INTO uy.t VALUES (2, 9, 'new');
SELECT length(dolt_commit('-am', 'uy theirs')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('uy_side'))[3:4];
SELECT * FROM uy.t ORDER BY pk;
SELECT violating_row::text, violation_info->>'Name' AS index
FROM dolt_constraint_violations('uy.t') ORDER BY 1;
ROLLBACK;
SELECT dolt_branch('-D', 'uy_side');

-- a column named t; NULLS NOT DISTINCT; a partial index (its predicate
-- held by both rows); and an index's own equality: under record_image_ops
-- (1.0) and (1.00) are different values, so they don't collide
CREATE TYPE uy.n AS (v numeric);
CREATE TABLE uy.x (pk int PRIMARY KEY, t int UNIQUE, n int UNIQUE NULLS NOT DISTINCT,
	p int, r uy.n);
CREATE UNIQUE INDEX x_p_part ON uy.x (p) WHERE p > 0;
CREATE UNIQUE INDEX x_r_image ON uy.x (r record_image_ops);
INSERT INTO uy.x VALUES (1, 1, 1, 1, ROW(1));
SELECT length(dolt_commit('-Am', 'uy x base')::text) AS committed;
SELECT dolt_branch('uy_x');
INSERT INTO uy.x VALUES (10, 7, NULL, 5, ROW(1.0)), (11, 8, 11, -1, ROW(2));
SELECT length(dolt_commit('-am', 'uy x ours')::text) AS committed;
SELECT dolt_checkout('uy_x');
-- (20: t with 10; 21: NULL n with 10; 22: p 5 with 10; 23: p -1 is out of
-- the index; 24: (1.00) isn't (1.0) by its image; 25: (1.0) is)
INSERT INTO uy.x VALUES (20, 7, 20, 20, ROW(20)), (21, 21, NULL, 21, ROW(21)),
	(22, 22, 22, 5, ROW(22)), (23, 23, 23, -1, ROW(23)), (24, 24, 24, 24, ROW(1.00)),
	(25, 25, 25, 25, ROW(1.0));
SELECT length(dolt_commit('-am', 'uy x theirs')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('uy_x'))[3:4];
SELECT pk FROM uy.x ORDER BY pk;
SELECT violating_row->>'pk' AS pk, violation_info->>'Name' AS index
FROM dolt_constraint_violations('uy.x') ORDER BY 1, 2;
ROLLBACK;
SELECT dolt_branch('-D', 'uy_x');

-- a row collided with that row security hides is found, as a foreign
-- key's checks find rows
CREATE TABLE uy.s (pk int PRIMARY KEY, a int UNIQUE, c text);
ALTER TABLE uy.s ENABLE ROW LEVEL SECURITY;
ALTER TABLE uy.s FORCE ROW LEVEL SECURITY;
CREATE POLICY sel ON uy.s FOR SELECT USING (c <> 'hidden');
CREATE POLICY ins ON uy.s FOR INSERT WITH CHECK (true);
CREATE POLICY upd ON uy.s FOR UPDATE USING (true);
CREATE POLICY del ON uy.s FOR DELETE USING (true);
INSERT INTO uy.s VALUES (1, 1, 'base');
SELECT length(dolt_commit('-Am', 'uy s base')::text) AS committed;
SELECT dolt_branch('uy_s');
INSERT INTO uy.s VALUES (2, 7, 'hidden');
SELECT length(dolt_commit('-am', 'uy s ours')::text) AS committed;
SELECT dolt_checkout('uy_s');
INSERT INTO uy.s VALUES (3, 7, 'theirs');
SELECT length(dolt_commit('-am', 'uy s theirs')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('uy_s'))[3:4];
SELECT violating_row::text FROM dolt_constraint_violations('uy.s') ORDER BY 1;
ROLLBACK;
SELECT dolt_branch('-D', 'uy_s');
DROP SCHEMA uy CASCADE;

SELECT dolt_branch('-D', 'feature');
DROP SCHEMA cv CASCADE;
DROP ROLE regress_cv_owner;
