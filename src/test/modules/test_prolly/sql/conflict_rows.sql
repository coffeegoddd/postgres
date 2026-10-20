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

-- A conflicted table's versions are found by the name it had as the merge
-- recorded it: two tables' names swapped mid-merge are refused, not read
-- as each other's, until their names are back.
CREATE DATABASE regress_conflict_swap;
\c regress_conflict_swap
CREATE SCHEMA cs VERSIONED;
CREATE TABLE cs.a (pk int PRIMARY KEY, v text);
CREATE TABLE cs.b (pk int PRIMARY KEY, v text);
INSERT INTO cs.a VALUES (1, 'base'); INSERT INTO cs.b VALUES (1, 'b');
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('side');
SELECT dolt_checkout('side');
UPDATE cs.a SET v = 'theirs';
SELECT length(dolt_commit('-am', 'side')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE cs.a SET v = 'ours';
SELECT length(dolt_commit('-am', 'main')::text) AS committed;
SET dolt.allow_commit_conflicts = on;
SELECT dolt_merge('side') IS NOT NULL AS merging;
SELECT * FROM dolt_conflicts;
ALTER TABLE cs.a RENAME TO tmp;
ALTER TABLE cs.b RENAME TO a;
ALTER TABLE cs.tmp RENAME TO b;
SELECT count(*) FROM dolt_conflicts(NULL::cs.b);
SELECT dolt_conflicts_resolve('--theirs', 'cs.b');
SELECT dolt_conflicts_resolve('--theirs', 'cs.a');
ALTER TABLE cs.b RENAME TO tmp;
ALTER TABLE cs.a RENAME TO b;
ALTER TABLE cs.tmp RENAME TO a;
SELECT our_diff_type, their_diff_type FROM dolt_conflicts(NULL::cs.a);
SELECT dolt_conflicts_resolve('--theirs', 'cs.a');
SELECT * FROM cs.a;
SELECT * FROM cs.b;
\c contrib_regression
DROP DATABASE regress_conflict_swap;

-- A conflicted partition detached and its partitioned table dropped: its
-- versions in the merge are a partition's, keyed by its parent's key, and
-- it now has a key of its own.  The summary lists it (its count unknown),
-- its rows are refused with why, and attached again they're there.
CREATE DATABASE regress_conflict_detached;
\c regress_conflict_detached
CREATE SCHEMA pt VERSIONED;
CREATE TABLE pt.p (id int PRIMARY KEY, v text) PARTITION BY RANGE (id);
CREATE TABLE pt.p1 PARTITION OF pt.p FOR VALUES FROM (0) TO (100);
CREATE TABLE pt.q (id int PRIMARY KEY, v text);
INSERT INTO pt.p VALUES (1, 'base'); INSERT INTO pt.q VALUES (1, 'base');
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('side');
SELECT dolt_checkout('side');
UPDATE pt.p SET v = 'theirs'; UPDATE pt.q SET v = 'theirs';
SELECT length(dolt_commit('-am', 'side')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE pt.p SET v = 'ours'; UPDATE pt.q SET v = 'ours';
SELECT length(dolt_commit('-am', 'main')::text) AS committed;
SET dolt.allow_commit_conflicts = on;
SELECT dolt_merge('side') IS NOT NULL AS merging;
SELECT * FROM dolt_conflicts ORDER BY 1;
ALTER TABLE pt.p DETACH PARTITION pt.p1;
DROP TABLE pt.p;
SELECT * FROM dolt_conflicts ORDER BY 1;
SELECT count(*) FROM dolt_conflicts(NULL::pt.p1);
SELECT dolt_conflicts_resolve('--theirs', 'pt.q');
SELECT * FROM pt.q;
\c contrib_regression
DROP DATABASE regress_conflict_detached;
