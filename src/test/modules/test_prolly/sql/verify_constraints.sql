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

-- Without --all, as in Dolt, only what changed since the head commit is
-- checked: rows added or updated, and rows referencing a parent row
-- removed or updated; a table made or redefined since, whole.  --all
-- checks every row.
CREATE SCHEMA vd VERSIONED;
CREATE TABLE vd.parent (pk int PRIMARY KEY, v int UNIQUE);
CREATE TABLE vd.child (pk int PRIMARY KEY, v int REFERENCES vd.parent (v));
CREATE TABLE vd.k (a int, b int);
INSERT INTO vd.parent VALUES (1, 1), (2, 2), (3, 3);
SET session_replication_role = replica;
INSERT INTO vd.child VALUES (1, 1), (2, 2), (3, 3), (9, 9);
RESET session_replication_role;
INSERT INTO vd.k VALUES (1, 2), (5, 4), (8, 7);
ALTER TABLE vd.k ADD CONSTRAINT k_chk CHECK (a < b) NOT VALID;
-- (made since the head: whole)
SELECT dolt_verify_constraints('-o');
SELECT length(dolt_commit('-Am', 'vd')::text) AS committed;
SELECT dolt_verify_constraints('-o');
SELECT dolt_verify_constraints('-ao');

SET dolt.allow_commit_conflicts = on;
-- a child added, a parent deleted, one's key changed, one's other column
SET session_replication_role = replica;
INSERT INTO vd.child VALUES (8, 8);
DELETE FROM vd.parent WHERE pk = 1;
UPDATE vd.parent SET v = 20 WHERE pk = 2;
UPDATE vd.parent SET pk = 30 WHERE pk = 3;
RESET session_replication_role;
-- (vd.k's rows breaking its check are as committed: not checked again;
-- Postgres checks a row written against it, validated or not)
BEGIN;
SELECT dolt_verify_constraints();
SELECT * FROM dolt_constraint_violations() ORDER BY table_name;
SELECT violation_type, violating_row FROM dolt_constraint_violations('vd.child')
ORDER BY violating_row->>'pk';
SELECT violating_row FROM dolt_constraint_violations('vd.k')
ORDER BY violating_row->>'a';
ROLLBACK;
BEGIN;
SELECT dolt_verify_constraints('--all', 'vd.child', 'vd.k');
SELECT * FROM dolt_constraint_violations() ORDER BY table_name;
ROLLBACK;
-- (a row of the transaction's own is a change too)
BEGIN;
SET LOCAL session_replication_role = replica;
INSERT INTO vd.child VALUES (7, 7);
SELECT dolt_verify_constraints('-o', 'vd.child');
ROLLBACK;
-- nothing changed, nothing found
SELECT dolt_reset('--hard');
SELECT dolt_verify_constraints('-o');
SELECT dolt_verify_constraints('-ao');
RESET dolt.allow_commit_conflicts;

DROP SCHEMA vd CASCADE;

-- Violations recorded are checked again without --all, changed or not:
-- one still holding is kept (and the status says so), one fixed goes.
CREATE SCHEMA vr VERSIONED;
SET dolt.allow_commit_conflicts = on;
CREATE TABLE vr.parent (id int PRIMARY KEY);
CREATE TABLE vr.child (id int PRIMARY KEY, p int REFERENCES vr.parent);
INSERT INTO vr.parent VALUES (1);
SET session_replication_role = replica;
INSERT INTO vr.child VALUES (1, 1), (2, 2), (3, 3);
RESET session_replication_role;
SELECT dolt_verify_constraints('vr.child');
SELECT length(dolt_commit('-Am', 'vr')::text) AS committed;
SELECT * FROM dolt_constraint_violations();
-- (nothing changed since the head)
SELECT dolt_verify_constraints();
SELECT * FROM dolt_constraint_violations();
-- (one recorded fixed, the other unchanged)
UPDATE vr.child SET p = 1 WHERE id = 3;
INSERT INTO vr.parent VALUES (5);
SELECT dolt_verify_constraints('vr.child');
SELECT violating_row FROM dolt_constraint_violations('vr.child');
SELECT dolt_reset('--hard');
-- A table redefined since is checked whole, whatever else of it changed.
CREATE TABLE vr.k (id int PRIMARY KEY, a int, b int);
INSERT INTO vr.k VALUES (1, 1, 2), (2, 5, 4), (3, 8, 7);
SELECT length(dolt_commit('-Am', 'vr.k')::text) AS committed;
ALTER TABLE vr.k ADD CONSTRAINT k_chk CHECK (a < b) NOT VALID;
UPDATE vr.k SET b = 3 WHERE id = 1;
SELECT dolt_verify_constraints('-o', 'vr.k');
SELECT dolt_verify_constraints('vr.k');
SELECT violating_row FROM dolt_constraint_violations('vr.k')
ORDER BY violating_row->>'id';
SELECT dolt_reset('--hard');
-- (so is a partitioned table one of whose partitions was, though another
-- partition's rows changed too)
CREATE TABLE vr.pk (id int PRIMARY KEY, a int, b int) PARTITION BY RANGE (id);
CREATE TABLE vr.pk1 PARTITION OF vr.pk FOR VALUES FROM (0) TO (10);
CREATE TABLE vr.pk2 PARTITION OF vr.pk FOR VALUES FROM (10) TO (20);
INSERT INTO vr.pk VALUES (1, 1, 2), (2, 5, 4), (11, 8, 7);
ALTER TABLE vr.pk ADD CONSTRAINT pk_chk CHECK (a < b) NOT VALID;
SELECT length(dolt_commit('-Am', 'vr.pk')::text) AS committed;
ALTER TABLE vr.pk2 ADD CONSTRAINT pk2_chk CHECK (id > 0);
UPDATE vr.pk SET b = 3 WHERE id = 1;
SELECT dolt_verify_constraints('vr.pk');
SELECT violating_row FROM dolt_constraint_violations('vr.pk')
ORDER BY (violating_row->>'id')::int;
SELECT dolt_reset('--hard');
-- Rows are matched by their keys, never by how the whole row prints: a
-- violation recorded is found again after its row changed otherwise, or
-- under another TimeZone; a json value no jsonb takes, or a virtual
-- generated column, doesn't stop a changed row matching.
CREATE TABLE vr.tp (id int PRIMARY KEY);
CREATE TABLE vr.tc (id int PRIMARY KEY, p int REFERENCES vr.tp, note text,
                    at timestamptz);
SET TimeZone = 'UTC';
SET session_replication_role = replica;
INSERT INTO vr.tc VALUES (1, 1, NULL, '2026-01-01 12:00+00'),
                         (2, 2, NULL, '2026-01-01 12:00+00');
RESET session_replication_role;
SELECT dolt_verify_constraints('vr.tc');
UPDATE vr.tc SET note = 'still an orphan' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'vr.tc')::text) AS committed;
SET TimeZone = 'America/New_York';
SELECT dolt_verify_constraints();
SELECT violating_row->>'id' AS id FROM dolt_constraint_violations('vr.tc')
ORDER BY 1;
RESET TimeZone;
SELECT dolt_reset('--hard');
DELETE FROM vr.tc;
SELECT dolt_verify_constraints('vr.tc');
SELECT length(dolt_commit('-Am', 'vr.tc empty')::text) AS committed;
CREATE TABLE vr.jc (id int PRIMARY KEY, p int REFERENCES vr.tp, doc json, s json);
CREATE TABLE vr.gc (id int PRIMARY KEY, p int REFERENCES vr.tp,
                    g int GENERATED ALWAYS AS (id * 2) VIRTUAL);
INSERT INTO vr.tp VALUES (1), (2);
INSERT INTO vr.jc VALUES (1, 1, '{"a": "\u0000"}', '"\u0000"');
INSERT INTO vr.gc VALUES (1, 1);
SELECT length(dolt_commit('-Am', 'vr.jc, vr.gc')::text) AS committed;
UPDATE vr.jc SET p = 2 WHERE id = 1;
SET session_replication_role = replica;
INSERT INTO vr.jc VALUES (2, 9, '"\u0000"', NULL);
INSERT INTO vr.gc VALUES (2, 9);
RESET session_replication_role;
SELECT dolt_verify_constraints('-o');
SELECT dolt_verify_constraints('vr.jc', 'vr.gc');
SELECT * FROM dolt_constraint_violations() ORDER BY table_name;
SELECT violating_row->>'id' AS id FROM dolt_constraint_violations('vr.gc');
SELECT dolt_reset('--hard');
-- However many rows changed, the same are found: a batch of keys at a
-- time, not the whole table once there are many (an orphan committed
-- before isn't found again).
CREATE TABLE vr.bc (id int PRIMARY KEY, p int REFERENCES vr.tp);
SET session_replication_role = replica;
INSERT INTO vr.bc VALUES (0, 9);
RESET session_replication_role;
SELECT length(dolt_commit('-Am', 'vr.bc')::text) AS committed;
SET session_replication_role = replica;
INSERT INTO vr.bc SELECT g, CASE WHEN g IN (1, 1000, 1001, 2000, 2001, 2400) THEN 9 ELSE 1 END
FROM generate_series(1, 2500) g;
RESET session_replication_role;
SELECT dolt_verify_constraints('vr.bc');
SELECT violating_row->>'id' AS id FROM dolt_constraint_violations('vr.bc')
ORDER BY (violating_row->>'id')::int;
SELECT dolt_reset('--hard');
-- A changed table with no unique index is checked whole; a changed row
-- with a value out of line, in its key or not, is read for its key; a
-- float key matches whatever extra_float_digits says.
CREATE TABLE vr.nk (a int, p int REFERENCES vr.tp);
CREATE TABLE vr.lc (id int PRIMARY KEY, p int REFERENCES vr.tp, big text);
CREATE TABLE vr.lk (k text PRIMARY KEY, p int REFERENCES vr.tp);
CREATE TABLE vr.fk (k float8 PRIMARY KEY, p int REFERENCES vr.tp);
SELECT length(dolt_commit('-Am', 'vr.nk, vr.lc, vr.lk, vr.fk')::text) AS committed;
SET session_replication_role = replica;
INSERT INTO vr.nk VALUES (1, 9);
INSERT INTO vr.lc SELECT 1, 9, string_agg(md5(g::text), '') FROM generate_series(1, 70) g;
INSERT INTO vr.lc SELECT 2, 1, string_agg(md5(g::text), '') FROM generate_series(1, 160) g;
INSERT INTO vr.lk SELECT string_agg(md5(g::text), '') || 'k', 9 FROM generate_series(1, 80) g;
INSERT INTO vr.lk VALUES ('short', 1);
INSERT INTO vr.fk VALUES (0.1, 9), (1.0 / 3, 9), (2, 1);
RESET session_replication_role;
SELECT dolt_verify_constraints('-o', 'vr.lc');
SELECT dolt_verify_constraints('-o', 'vr.lk');
SET extra_float_digits = 0;
SELECT dolt_verify_constraints('vr.fk');
SELECT violating_row->>'p' AS p FROM dolt_constraint_violations('vr.fk');
RESET extra_float_digits;
SELECT dolt_verify_constraints('-o', 'vr.nk');
SELECT dolt_verify_constraints('vr.lc', 'vr.lk', 'vr.nk');
SELECT * FROM dolt_constraint_violations() ORDER BY table_name;
SELECT dolt_reset('--hard');
RESET dolt.allow_commit_conflicts;
DROP SCHEMA vr CASCADE;

-- On a branch with no commit yet, every row is a change.
CREATE DATABASE vc_fresh TEMPLATE template0;
\c vc_fresh
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.parent (id int PRIMARY KEY);
CREATE TABLE app.child (id int PRIMARY KEY, p int REFERENCES app.parent);
SET session_replication_role = replica;
INSERT INTO app.child VALUES (1, 1);
RESET session_replication_role;
SELECT dolt_verify_constraints('-o');
SELECT dolt_verify_constraints('-o', 'app.child');
\c contrib_regression
DROP DATABASE vc_fresh;
