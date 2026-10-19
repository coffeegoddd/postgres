-- A merge's conflicts list the branch's row as it is now, as Dolt's
-- dolt_conflicts_<t> reads it from the working set: none once deleted
-- (or the table truncated), the new values once updated.  The conflicts
-- themselves are the merge's, kept until resolved; resolving with theirs
-- writes their row over the branch's row now.

CREATE SCHEMA cr VERSIONED;
CREATE TABLE cr.t (pk int PRIMARY KEY, col1 int);
SELECT length(dolt_commit('-Am', 'create table')::text) AS committed;
SELECT dolt_branch('cr_other');
SELECT dolt_checkout('cr_other') IS NOT NULL AS on_other;
INSERT INTO cr.t VALUES (1, 100), (2, 200), (3, 300);
SELECT length(dolt_commit('-am', 'other')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
INSERT INTO cr.t VALUES (1, -100), (2, -200), (3, -300);
SELECT length(dolt_commit('-am', 'main')::text) AS committed;
SET dolt.allow_commit_conflicts = on;

-- deleted, updated, kept
SELECT (dolt_merge('cr_other'))[3] AS conflicts;
DELETE FROM cr.t WHERE pk = 1;
UPDATE cr.t SET col1 = -222 WHERE pk = 2;
SELECT (base_row).pk AS base_pk, (our_row).pk AS our_pk, (our_row).col1 AS our_col1,
       (their_row).pk AS their_pk, (their_row).col1 AS their_col1
FROM dolt_conflicts(NULL::cr.t) ORDER BY (their_row).pk;
SELECT * FROM dolt_conflicts;

-- in the transaction that deletes them, too
BEGIN;
DELETE FROM cr.t;
SELECT (our_row).pk AS our_pk, (their_row).pk AS their_pk
FROM dolt_conflicts(NULL::cr.t) ORDER BY (their_row).pk;
ROLLBACK;

-- resolved with theirs: their rows written over the branch's now (one
-- deleted, one updated, one as the merge left it)
SELECT dolt_conflicts_resolve('--theirs', 'cr.t');
SELECT * FROM cr.t ORDER BY pk;
SELECT * FROM dolt_conflicts;
SELECT dolt_reset('--hard');

-- truncated, then resolved with ours: nothing to write
SELECT (dolt_merge('cr_other'))[3] AS conflicts;
TRUNCATE cr.t;
SELECT (our_row).pk AS our_pk, (their_row).pk AS their_pk
FROM dolt_conflicts(NULL::cr.t) ORDER BY (their_row).pk;
SELECT dolt_conflicts_resolve('--ours', 'cr.t');
SELECT count(*) AS rows FROM cr.t;
SELECT dolt_reset('--hard');

-- A table renamed since the merge began can't have its conflicts read
-- (none of the merge's versions has it by its name now): an error, and
-- the summary lists it with its count unknown, the others' still shown.
CREATE TABLE cr.a (pk int PRIMARY KEY, col1 int);
CREATE TABLE cr.b (pk int PRIMARY KEY, col1 int);
SELECT length(dolt_commit('-Am', 'a, b')::text) AS committed;
SELECT dolt_branch('cr_ab');
SELECT dolt_checkout('cr_ab') IS NOT NULL AS on_ab;
INSERT INTO cr.a VALUES (1, 100);
INSERT INTO cr.b VALUES (1, 100);
SELECT length(dolt_commit('-am', 'ab')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
INSERT INTO cr.a VALUES (1, -100);
INSERT INTO cr.b VALUES (1, -100);
SELECT length(dolt_commit('-am', 'main ab')::text) AS committed;
SELECT (dolt_merge('cr_ab'))[3] AS conflicts;
ALTER TABLE cr.a RENAME TO a2;
SELECT * FROM dolt_conflicts ORDER BY table_name;
SELECT * FROM dolt_conflicts(NULL::cr.a2);
SELECT dolt_conflicts_resolve('--theirs', 'cr.a2');
ALTER TABLE cr.a2 RENAME TO a;
SELECT * FROM dolt_conflicts ORDER BY table_name;
-- (one the working set leaves out, a partition whose columns are in
-- another order than its partitioned table's: walked against ours' rows,
-- the branch's as committed)
CREATE TABLE cr.p (col1 int, pk int) PARTITION BY RANGE (pk);
ALTER TABLE cr.p ATTACH PARTITION cr.a FOR VALUES FROM (0) TO (100);
SELECT * FROM dolt_conflicts ORDER BY table_name;
SELECT (our_row).col1 AS ours, (their_row).col1 AS theirs FROM dolt_conflicts(NULL::cr.a);
SELECT dolt_conflicts_resolve('--theirs', 'cr.a');
SELECT * FROM cr.a;
SELECT * FROM dolt_conflicts ORDER BY table_name;
SELECT dolt_reset('--hard');
SELECT dolt_branch('-D', 'cr_ab');

RESET dolt.allow_commit_conflicts;
SELECT dolt_branch('-D', 'cr_other');
DROP SCHEMA cr CASCADE;
SELECT length(dolt_commit('-Am', 'without cr')::text) AS committed;
