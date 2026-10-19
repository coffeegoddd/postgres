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

SELECT dolt_branch('-D', 'other');
SELECT dolt_branch('-D', 'other2');
SELECT dolt_branch('-D', 'other3');
SELECT dolt_branch('-D', 'fine');
DROP SCHEMA dv CASCADE;
