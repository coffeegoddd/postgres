-- A merge checks the rows it writes against their columns' domains'
-- constraints, as Postgres checks a value input or cast to one: a row
-- breaking one isn't written, and is recorded as a check constraint or
-- not-null violation, as one breaking a table's constraint is (the
-- replication applier it writes with doesn't check domains).

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA dv VERSIONED;
CREATE DOMAIN dv.posint AS int;
CREATE DOMAIN dv.small AS dv.posint;	-- over another
CREATE DOMAIN dv.label AS text;
CREATE TABLE dv.t (pk int PRIMARY KEY, v dv.posint, s dv.small, l dv.label);
INSERT INTO dv.t VALUES (1, 1, 1, 'one'), (2, 2, 2, 'two');
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('other');
SELECT dolt_checkout('other');
INSERT INTO dv.t VALUES (3, -3, 3, 'three');	-- v breaks it
INSERT INTO dv.t VALUES (4, 4, -4, 'four');	-- s, by its base's
INSERT INTO dv.t VALUES (5, 5, 5, NULL);		-- l
INSERT INTO dv.t VALUES (6, 6, 6, 'six');		-- none
UPDATE dv.t SET v = 0 WHERE pk = 1;			-- an update breaking it
UPDATE dv.t SET l = 'TWO' WHERE pk = 2;		-- one that doesn't
SELECT length(dolt_commit('-am', 'theirs')::text) AS committed;
SELECT dolt_checkout('main');
ALTER DOMAIN dv.posint ADD CONSTRAINT pos CHECK (VALUE > 0);
ALTER DOMAIN dv.label SET NOT NULL;
SELECT length(dolt_commit('-am', 'ours: the domains constrained')::text) AS committed;

-- Outside a transaction block, the merge is rolled back.
SELECT dolt_merge('other');
SELECT * FROM dv.t ORDER BY pk;

-- In one, it stops with them recorded, the rows not written (the branch's
-- version of one updated kept).
BEGIN;
SELECT (dolt_merge('other'))[3:4];
SELECT violation_type, violating_row, violation_info
FROM dolt_constraint_violations('dv.t')
ORDER BY violating_row->>'pk';
SELECT * FROM dv.t ORDER BY pk;
-- fixed, and checked again
INSERT INTO dv.t VALUES (3, 3, 3, 'three');
SELECT dolt_verify_constraints('dv.t');
SELECT count(*) AS left_over FROM dolt_constraint_violations();
SELECT length(dolt_commit('-am', 'merged')::text) AS committed;
COMMIT;
SELECT * FROM dv.t ORDER BY pk;

-- The values an array or a composite is made of are checked by their
-- domain too (a constraint not validated, as ALTER DOMAIN validates none
-- there).
CREATE DOMAIN dv.elem AS int;
CREATE TYPE dv.pair AS (a dv.elem, b int);
CREATE TABLE dv.c (pk int PRIMARY KEY, arr dv.elem[], p dv.pair);
INSERT INTO dv.c VALUES (1, '{1}', '(1,1)');
SELECT length(dolt_commit('-Am', 'containers')::text) AS committed;
SELECT dolt_branch('other2');
SELECT dolt_checkout('other2');
INSERT INTO dv.c VALUES (2, '{2,-2}', '(2,2)');	-- an element
INSERT INTO dv.c VALUES (3, '{3}', '(-3,3)');	-- a field
INSERT INTO dv.c VALUES (4, '{4}', '(4,-4)');	-- none
INSERT INTO dv.c VALUES (5, '{5,NULL}', '(,5)');	-- nulls, which it allows
SELECT length(dolt_commit('-am', 'theirs')::text) AS committed;
SELECT dolt_checkout('main');
ALTER DOMAIN dv.elem ADD CONSTRAINT pos CHECK (VALUE > 0) NOT VALID;
SELECT length(dolt_commit('-am', 'ours: elem constrained')::text) AS committed;
BEGIN;
SELECT (dolt_merge('other2'))[3:4];
SELECT violating_row->>'pk' AS pk, violation_type, violation_info
FROM dolt_constraint_violations('dv.c')
ORDER BY violating_row->>'pk';
SELECT pk FROM dv.c ORDER BY pk;
ROLLBACK;

-- A domain constrained on theirs: the branch's rows the merge rewrites
-- are checked too, a row breaking it recorded (and kept as the domain's
-- move converted it: a value that isn't one of it now is null).
CREATE DOMAIN dv.ours AS int;
CREATE TABLE dv.u (pk int PRIMARY KEY, v dv.ours);
INSERT INTO dv.u VALUES (1, 1);
SELECT length(dolt_commit('-Am', 'u')::text) AS committed;
SELECT dolt_branch('other3');
INSERT INTO dv.u VALUES (2, -2), (3, 3);
SELECT length(dolt_commit('-am', 'ours: u rows')::text) AS committed;
SELECT dolt_checkout('other3');
ALTER DOMAIN dv.ours ADD CONSTRAINT pos CHECK (VALUE > 0);
INSERT INTO dv.u VALUES (4, 4);
SELECT length(dolt_commit('-am', 'theirs: ours constrained')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('other3'))[3:4];
SELECT violation_type, violating_row, violation_info
FROM dolt_constraint_violations('dv.u');
SELECT * FROM dv.u ORDER BY pk;
ROLLBACK;

-- Only the values a write changes are checked: a stored value a NOT VALID
-- constraint allows stays (as an UPDATE of another column leaves it), with
-- theirs' change to another column written, the row updated in place or
-- (a second unique index) removed and inserted again.  A value theirs
-- changes is checked: refused, the violation is recorded and the branch's
-- row kept, in place (dv.n) or put back (dv.nu).  In a keyless
-- table, a changed row is removed and another inserted: a value a row
-- removed held isn't checked, theirs' rows written as ours' are kept
-- (dv.k) or ours changed rows too (dv.k2).
CREATE DOMAIN dv.nv AS int;
CREATE TABLE dv.n (pk int PRIMARY KEY, v dv.nv, w int);
CREATE TABLE dv.nu (pk int PRIMARY KEY, v dv.nv, w int, u int UNIQUE);
INSERT INTO dv.n VALUES (1, -1, 10), (2, -2, 20);
INSERT INTO dv.nu VALUES (1, -1, 10, 1), (2, -2, 20, 2);
CREATE TABLE dv.k (v dv.nv, w int);
CREATE TABLE dv.k2 (v dv.nv, w int);
INSERT INTO dv.k VALUES (-1, 10), (5, 50);
INSERT INTO dv.k2 VALUES (-1, 10), (5, 50);
SELECT length(dolt_commit('-Am', 'n')::text) AS committed;
SELECT dolt_branch('other4');
ALTER DOMAIN dv.nv ADD CONSTRAINT pos CHECK (VALUE > 0) NOT VALID;
INSERT INTO dv.k2 VALUES (7, 70);
SELECT length(dolt_commit('-am', 'ours: nv constrained, not valid')::text) AS committed;
SELECT dolt_checkout('other4');
UPDATE dv.n SET w = 11 WHERE pk = 1;
UPDATE dv.nu SET w = 11 WHERE pk = 1;
UPDATE dv.k SET w = 11 WHERE v = -1;
UPDATE dv.k2 SET w = 11 WHERE v = -1;
UPDATE dv.n SET v = -3 WHERE pk = 2;		-- a change breaking it
UPDATE dv.nu SET v = -3 WHERE pk = 2;
SELECT length(dolt_commit('-am', 'theirs: w, v')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('other4'))[3:4];
SELECT 'n' AS t, violating_row->>'pk' AS pk, violation_type, violation_info
FROM dolt_constraint_violations('dv.n')
UNION ALL
SELECT 'nu', violating_row->>'pk' AS pk, violation_type, violation_info
FROM dolt_constraint_violations('dv.nu')
ORDER BY t, pk;
SELECT * FROM dv.n ORDER BY pk;
SELECT * FROM dv.nu ORDER BY pk;
SELECT count(*) AS none FROM dolt_constraint_violations('dv.k');
SELECT count(*) AS none FROM dolt_constraint_violations('dv.k2');
SELECT * FROM dv.k ORDER BY v, w;
SELECT * FROM dv.k2 ORDER BY v, w;
ROLLBACK;

-- A domain over another, each with a constraint of the same name: the one
-- broken is described, the base's.
CREATE DOMAIN dv.lower AS int;
CREATE DOMAIN dv.upper AS dv.lower;
CREATE TABLE dv.s (pk int PRIMARY KEY, v dv.upper);
INSERT INTO dv.s VALUES (1, 1);
SELECT length(dolt_commit('-Am', 's')::text) AS committed;
SELECT dolt_branch('other5');
ALTER DOMAIN dv.lower ADD CONSTRAINT c CHECK (VALUE > 0);
ALTER DOMAIN dv.upper ADD CONSTRAINT c CHECK (VALUE < 100);
SELECT length(dolt_commit('-am', 'ours: lower and upper constrained')::text) AS committed;
SELECT dolt_checkout('other5');
INSERT INTO dv.s VALUES (2, -5), (3, 500), (4, 50);
SELECT length(dolt_commit('-am', 'theirs: s rows')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('other5'))[3:4];
SELECT violating_row->>'pk' AS pk, violation_type, violation_info
FROM dolt_constraint_violations('dv.s')
ORDER BY pk;
SELECT pk FROM dv.s ORDER BY pk;
ROLLBACK;

-- Rows that meet the domains merge as ever.
SELECT dolt_branch('fine');
SELECT dolt_checkout('fine');
INSERT INTO dv.t VALUES (7, 7, 7, 'seven');
INSERT INTO dv.c VALUES (7, '{7}', '(7,7)');
SELECT length(dolt_commit('-am', 'fine')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('fine'))[3:4];
SELECT count(*) FROM dolt_constraint_violations();
SELECT pk FROM dv.t ORDER BY pk;
SELECT pk FROM dv.c ORDER BY pk;
SELECT pk FROM dv.u ORDER BY pk;

-- Conflict resolution checks domains too, before writing any taken row.
-- Merge and resolution run in separate transactions, and failed resolutions
-- leave both the rows and the conflicts intact.
CREATE DOMAIN dv.rint AS int;
CREATE DOMAIN dv.rlabel AS text;
CREATE DOMAIN dv.relem AS int;
CREATE TYPE dv.rpair AS (a dv.relem);
CREATE DOMAIN dv.rgrand AS int;
CREATE TABLE dv.r (pk int PRIMARY KEY, v dv.rint);
CREATE TABLE dv.ru (pk int PRIMARY KEY, v dv.rint UNIQUE);
CREATE TABLE dv.rn (pk int PRIMARY KEY, v dv.rlabel);
CREATE TABLE dv.ra (pk int PRIMARY KEY, v dv.relem[]);
CREATE TABLE dv.rc (pk int PRIMARY KEY, v dv.rpair);
CREATE TABLE dv.rg (pk int PRIMARY KEY, v dv.rgrand, w int);
CREATE TABLE dv.rv (pk int PRIMARY KEY, v dv.rint);
INSERT INTO dv.rg VALUES (1, -1, 0);
SELECT length(dolt_commit('-Am', 'resolution base')::text) AS committed;
SELECT dolt_branch('resolve_domains');
SELECT dolt_checkout('resolve_domains');
INSERT INTO dv.r VALUES (1, 3), (2, -2);
INSERT INTO dv.ru VALUES (1, -1);
INSERT INTO dv.rn VALUES (1, NULL);
INSERT INTO dv.ra VALUES (1, '{-1}');
INSERT INTO dv.rc VALUES (1, '(-1)');
UPDATE dv.rg SET w = 20;
INSERT INTO dv.rv VALUES (1, 3), (2, 4);
SELECT length(dolt_commit('-am', 'theirs: conflicting domain values')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO dv.r VALUES (1, 1), (2, 2);
INSERT INTO dv.ru VALUES (1, 1);
INSERT INTO dv.rn VALUES (1, 'ours');
INSERT INTO dv.ra VALUES (1, '{1}');
INSERT INTO dv.rc VALUES (1, '(1)');
UPDATE dv.rg SET w = 10;
INSERT INTO dv.rv VALUES (1, 1), (2, 2);
ALTER DOMAIN dv.rint ADD CONSTRAINT positive CHECK (VALUE > 0);
ALTER DOMAIN dv.rlabel SET NOT NULL;
ALTER DOMAIN dv.relem ADD CONSTRAINT positive CHECK (VALUE > 0) NOT VALID;
ALTER DOMAIN dv.rgrand ADD CONSTRAINT positive CHECK (VALUE > 0) NOT VALID;
SELECT length(dolt_commit('-am', 'ours: conflicting rows and constraints')::text) AS committed;
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('resolve_domains'))[3] AS conflicts;

-- The first row would fit, but the second must not be written.  Neither
-- row changes, and the table still has its conflicts after the error.
SELECT dolt_conflicts_resolve('--theirs', 'dv.r');
SELECT * FROM dv.r ORDER BY pk;
SELECT count(*) AS conflicts_left FROM dolt_conflicts(NULL::dv.r);
-- A row deleted since the merge would be inserted, and is checked too.
DELETE FROM dv.r WHERE pk = 2;
SELECT dolt_conflicts_resolve('--theirs', 'dv.r');
SELECT * FROM dv.r ORDER BY pk;
SELECT dolt_conflicts_resolve('--ours', 'dv.r');

-- A second unique index makes resolution remove and reinsert the row.
SELECT dolt_conflicts_resolve('--theirs', 'dv.ru');
SELECT * FROM dv.ru;
SELECT dolt_conflicts_resolve('--ours', 'dv.ru');
SELECT dolt_conflicts_resolve('--theirs', 'dv.rn');
SELECT * FROM dv.rn;
SELECT dolt_conflicts_resolve('--ours', 'dv.rn');
SELECT dolt_conflicts_resolve('--theirs', 'dv.ra');
SELECT * FROM dv.ra;
SELECT dolt_conflicts_resolve('--ours', 'dv.ra');
SELECT dolt_conflicts_resolve('--theirs', 'dv.rc');
SELECT * FROM dv.rc;
SELECT dolt_conflicts_resolve('--ours', 'dv.rc');

-- An unchanged value grandfathered by NOT VALID remains allowed.  Compare
-- with the working row: changing it since the merge makes theirs' old
-- negative value a new, invalid value.  Rebuild the plans after an error
-- and a domain constraint change in the same transaction, too.
BEGIN;
UPDATE dv.rg SET v = 1;
SAVEPOINT before_resolution;
SELECT dolt_conflicts_resolve('--theirs', 'dv.rg');
ROLLBACK TO before_resolution;
SELECT * FROM dv.rg;
ALTER DOMAIN dv.rgrand DROP CONSTRAINT positive;
SELECT dolt_conflicts_resolve('--theirs', 'dv.rg');
SELECT * FROM dv.rg;
ROLLBACK;
SELECT dolt_conflicts_resolve('--theirs', 'dv.rg');
SELECT * FROM dv.rg;

-- Valid values still resolve, whether updating or reinserting a row
-- deleted from the working set, and the conflicts are cleared.
DELETE FROM dv.rv WHERE pk = 2;
SELECT dolt_conflicts_resolve('--theirs', 'dv.rv');
SELECT * FROM dv.rv ORDER BY pk;
SELECT count(*) AS conflicts_left FROM dolt_conflicts;
SELECT dolt_reset('--hard');
RESET dolt.allow_commit_conflicts;
SELECT dolt_branch('-D', 'resolve_domains');

SELECT dolt_branch('-D', 'other');
SELECT dolt_branch('-D', 'other2');
SELECT dolt_branch('-D', 'other3');
SELECT dolt_branch('-D', 'other4');
SELECT dolt_branch('-D', 'other5');
SELECT dolt_branch('-D', 'fine');
DROP SCHEMA dv CASCADE;
