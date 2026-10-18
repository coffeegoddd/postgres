-- dolt_verify_constraints for check and not-null constraints, and its
-- --all and --output-only, as in Dolt.  A table holds no row that breaks
-- a validated check or not-null constraint (Postgres checks each row
-- written), so the rows found break one not validated yet.

CREATE SCHEMA vc VERSIONED;
CREATE TABLE vc.t (pk int PRIMARY KEY, col1 int, col2 int);
INSERT INTO vc.t VALUES (1, 1, 2), (2, 5, 5), (3, 7, NULL);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
ALTER TABLE vc.t ADD CONSTRAINT t_chk CHECK (col1 <> col2) NOT VALID;
ALTER TABLE vc.t ADD CONSTRAINT t_col2_nn NOT NULL col2 NOT VALID;

-- Found and recorded, in a transaction block (else they can't commit).
BEGIN;
SELECT dolt_verify_constraints();
SELECT * FROM dolt_constraint_violations();
SELECT violation_type, violating_row, violation_info
FROM dolt_constraint_violations('vc.t') ORDER BY violation_type;
ROLLBACK;
SELECT dolt_verify_constraints();

-- --output-only reports them without recording them; --all checks every
-- row, as every check does.
SELECT dolt_verify_constraints('--output-only');
SELECT * FROM dolt_constraint_violations();
SELECT dolt_verify_constraints('-o', 'vc.t');
SELECT dolt_verify_constraints('--all', '--output-only');
SELECT dolt_verify_constraints('-ao');
SELECT dolt_verify_constraints('-oa');
SELECT dolt_verify_constraints('-aoa');
SELECT dolt_verify_constraints('-ax');
SELECT dolt_verify_constraints('-');
SELECT dolt_verify_constraints('--nope');

-- Fixed, nothing is found; and validated, a constraint is Postgres's.
UPDATE vc.t SET col2 = 6 WHERE pk = 2;
UPDATE vc.t SET col2 = 0 WHERE pk = 3;
SELECT dolt_verify_constraints();
SELECT dolt_verify_constraints('--all', 'vc.t');
ALTER TABLE vc.t VALIDATE CONSTRAINT t_chk;
ALTER TABLE vc.t VALIDATE CONSTRAINT t_col2_nn;
SELECT dolt_verify_constraints();

-- A table named is checked alone.
CREATE TABLE vc.u (a int);
INSERT INTO vc.u VALUES (-1);
ALTER TABLE vc.u ADD CONSTRAINT u_pos CHECK (a > 0) NOT VALID;
SELECT dolt_verify_constraints('-o', 'vc.t');
SELECT dolt_verify_constraints('-o', 'vc.u');

-- A partitioned table's check, unvalidated: each row breaking it found
-- once, through the partitioned table (not again through its partition's).
CREATE TABLE vc.parted (k int, v int) PARTITION BY RANGE (k);
CREATE TABLE vc.parted_1 PARTITION OF vc.parted FOR VALUES FROM (0) TO (10);
INSERT INTO vc.parted VALUES (1, -1), (2, 2);
ALTER TABLE vc.parted ADD CONSTRAINT parted_pos CHECK (v > 0) NOT VALID;
BEGIN;
SELECT dolt_verify_constraints('vc.parted', 'vc.parted_1');
SELECT * FROM dolt_constraint_violations();
SELECT violating_row FROM dolt_constraint_violations('vc.parted');
ROLLBACK;

DROP SCHEMA vc CASCADE;
