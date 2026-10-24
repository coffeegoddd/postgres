-- A writer with its tables' grants alone moves its writer branch to its
-- base's definitions, which the base's writers made, after the database's
-- owner changes another table's: a move drops and makes again every
-- table's policies, so a policy the move leaves as it is, or makes as the
-- base has it, runs as its table's owner; a move to the base's head runs
-- its definitions with the authority to make them; the rows take the
-- writer's own privileges either way (#935).
CREATE DATABASE regress_writer_definitions;
\c regress_writer_definitions
CREATE EXTENSION test_prolly;
CREATE SCHEMA s VERSIONED;
CREATE TABLE s.t (id int PRIMARY KEY, v text);
CREATE TABLE s.rls (id int PRIMARY KEY, who text);
CREATE TABLE s.other (id int PRIMARY KEY);
ALTER TABLE s.rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY p ON s.rls USING (who = current_user);
CREATE ROLE regress_wd_writer;
GRANT USAGE ON SCHEMA s TO regress_wd_writer;
GRANT ALL ON s.t TO regress_wd_writer;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_hashof('main') AS base_commit \gset
SELECT dolt_writer_mode('branch-auto');

-- the writer writes on its writer branch and commits there
SET ROLE regress_wd_writer;
SET dolt.writer = 'w';
INSERT INTO s.t VALUES (1, 'x');
SELECT active_branch();
SELECT length(dolt_commit('-am', 'mine')::text) AS committed;
RESET dolt.writer;
RESET ROLE;

-- the owner changes the policy, and another table, on main
SELECT dolt_checkout('main');
SET dolt.writer = 'owner';
ALTER POLICY p ON s.rls USING (who = 'nobody');
ALTER TABLE s.other ADD COLUMN c int;
SELECT (dolt_integrate('-m', 'owner''s definitions'))[2] AS fast_forward;
RESET dolt.writer;
SELECT dolt_checkout('main');
SELECT dolt_hashof('main') AS owner_commit \gset

-- the writer merges main in: the policy made as main has it, by its owner
SET ROLE regress_wd_writer;
SET dolt.writer = 'w';
SELECT dolt_checkout('writer/w/main');
SELECT (dolt_merge('-m', 'main in', 'main'))[3] AS conflicts;
SELECT polname, pg_get_expr(polqual, polrelid) AS qual FROM pg_policy
WHERE polrelid = 's.rls'::regclass;
-- reset to main: every definition main's, the writer's commit dropped
SELECT dolt_reset('--hard', 'main') IS NOT NULL AS reset;
SELECT count(*) AS c_column FROM pg_attribute
WHERE attrelid = 's.other'::regclass AND attname = 'c';
SELECT * FROM s.t ORDER BY id;
-- a move to another commit, whose definitions aren't the base's, takes the
-- owner's privileges still: the writer can't make another policy so
\set VERBOSITY terse
SELECT dolt_reset('--hard', :'base_commit');
\set VERBOSITY default
SELECT polname, pg_get_expr(polqual, polrelid) AS qual FROM pg_policy
WHERE polrelid = 's.rls'::regclass;
-- and integrates its own writes
INSERT INTO s.t VALUES (2, 'y');
SELECT length(dolt_commit('-am', 'mine again')::text) AS committed;
SELECT (dolt_integrate('-m', 'in'))[1] = dolt_hashof('main') AS integrated;
RESET dolt.writer;
RESET ROLE;
SELECT dolt_checkout('main');
SELECT * FROM s.t ORDER BY id;
SELECT dolt_writer_mode('shared');

\c contrib_regression
DROP DATABASE regress_writer_definitions;
DROP ROLE regress_wd_writer;
