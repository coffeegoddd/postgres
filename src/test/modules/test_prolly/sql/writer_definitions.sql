-- A writer with its tables' grants alone moves its writer branch to its
-- base's definitions, which the base's writers made, after the base's
-- owners change tables it doesn't own: a move drops and makes again every
-- table's policies, so a policy the move leaves as it is, or makes as the
-- base has it, runs as its table's owner; a move whose every definition
-- change is the base's runs its statements as the least role that may
-- make them (the owner of what it changes, the database's owner for what
-- it makes); the rows take the writer's own privileges either way (#935).
CREATE ROLE regress_wd_dbowner;
CREATE ROLE regress_wd_tabowner;
CREATE ROLE regress_wd_writer;
CREATE DATABASE regress_writer_definitions OWNER regress_wd_dbowner;
\c regress_writer_definitions
CREATE EXTENSION test_prolly;
CREATE SCHEMA s VERSIONED;
ALTER SCHEMA s OWNER TO regress_wd_dbowner;
GRANT USAGE, CREATE ON SCHEMA s TO regress_wd_tabowner;
CREATE TABLE s.t (id int PRIMARY KEY, v text);
CREATE TABLE s.rls (id int PRIMARY KEY, who text);
CREATE TABLE s.other (id int PRIMARY KEY);
ALTER TABLE s.rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY p ON s.rls USING (who = current_user);
ALTER TABLE s.rls OWNER TO regress_wd_tabowner;
ALTER TABLE s.other OWNER TO regress_wd_tabowner;
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

-- the tables' owner changes the policy, and another table, on main
SET dolt.writer = 'owner';
SELECT dolt_checkout('main');
ALTER POLICY p ON s.rls USING (who = 'nobody');
ALTER TABLE s.other ADD COLUMN c int;
SELECT (dolt_integrate('-m', 'owner''s definitions'))[2] AS fast_forward;
RESET dolt.writer;
SELECT dolt_checkout('main');

-- the writer merges main in: the policy and the column made as main has
-- them, by the tables' owner
SET ROLE regress_wd_writer;
SET dolt.writer = 'w';
SELECT dolt_checkout('writer/w/main');
SELECT (dolt_merge('-m', 'main in', 'main'))[3] AS conflicts;
SELECT polname, pg_get_expr(polqual, polrelid) AS qual FROM pg_policy
WHERE polrelid = 's.rls'::regclass;
SELECT count(*) AS c_column FROM pg_attribute
WHERE attrelid = 's.other'::regclass AND attname = 'c';
RESET dolt.writer;
RESET ROLE;

-- the database's owner makes a table on main; the writer's reset to main
-- makes it as the database's owner, not as anyone with more authority
SELECT dolt_checkout('main');
SET dolt.writer = 'owner';
SET ROLE regress_wd_dbowner;
CREATE TABLE s.made (id int PRIMARY KEY);
RESET ROLE;
SELECT (dolt_integrate('-m', 'made'))[2] AS fast_forward;
RESET dolt.writer;
SELECT dolt_checkout('main');
SET ROLE regress_wd_writer;
SET dolt.writer = 'w';
SELECT dolt_checkout('writer/w/main');
SELECT dolt_reset('--hard', 'main') IS NOT NULL AS reset;
SELECT relowner::regrole AS made_owner FROM pg_class WHERE oid = 's.made'::regclass;
SELECT * FROM s.t ORDER BY id;
-- a move to another commit, whose definitions aren't the base's, takes the
-- owner's privileges still: the writer can't make another policy so
\set VERBOSITY terse
SELECT dolt_reset('--hard', :'base_commit');
\set VERBOSITY default
SELECT polname, pg_get_expr(polqual, polrelid) AS qual FROM pg_policy
WHERE polrelid = 's.rls'::regclass;
RESET dolt.writer;
RESET ROLE;

-- nor a merge of a branch other than the base, whose definitions aren't
-- the base's
SELECT dolt_checkout('main');
SELECT dolt_branch('other', :'base_commit') IS NOT NULL AS made;
SET dolt.writer = 'other';
SELECT dolt_checkout('other');
ALTER TABLE s.other ADD COLUMN d int;
SELECT length(dolt_commit('-am', 'another column')::text) AS committed;
RESET dolt.writer;
SELECT dolt_checkout('main');
SET ROLE regress_wd_writer;
SET dolt.writer = 'w';
INSERT INTO s.t VALUES (2, 'y');
SELECT active_branch();
SELECT length(dolt_commit('-am', 'mine again')::text) AS committed;
\set VERBOSITY terse
SELECT dolt_merge('-m', 'other in', 'other');
\set VERBOSITY default
-- and integrates its own writes
SELECT (dolt_integrate('-m', 'in'))[1] = dolt_hashof('main') AS integrated;
RESET dolt.writer;
RESET ROLE;
SELECT dolt_checkout('main');
SELECT * FROM s.t ORDER BY id;
SELECT dolt_writer_mode('shared');

\c contrib_regression
DROP DATABASE regress_writer_definitions;
DROP ROLE regress_wd_writer, regress_wd_tabowner, regress_wd_dbowner;
