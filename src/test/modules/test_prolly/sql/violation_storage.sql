-- A branch's constraint violations are kept one entry each, added after
-- those it has, and a merge finds those again only once.  Set again, they
-- are replaced.  A branch renamed takes them with it; a branch deleted, or
-- reset, drops them.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE SCHEMA vs VERSIONED;
CREATE TABLE vs.parent (id int PRIMARY KEY);
CREATE TABLE vs.child (id int PRIMARY KEY, p int REFERENCES vs.parent);
CREATE TABLE vs.u (pk int PRIMARY KEY, b int);
INSERT INTO vs.parent VALUES (1), (2);
SELECT length(dolt_commit('-Am', 'vs base')::text) AS committed;
SELECT dolt_branch('vs_theirs');
SELECT dolt_branch('vs_more');
INSERT INTO vs.child VALUES (1, 1), (2, 2);
CREATE UNIQUE INDEX u_b ON vs.u (b);
INSERT INTO vs.u VALUES (10, 1);
SELECT length(dolt_commit('-Am', 'vs ours')::text) AS committed;
SELECT dolt_checkout('vs_theirs');
DELETE FROM vs.parent;
SELECT length(dolt_commit('-am', 'vs theirs')::text) AS committed;
SELECT dolt_checkout('vs_more');
INSERT INTO vs.parent VALUES (5);
INSERT INTO vs.u VALUES (11, 1);
SELECT length(dolt_commit('-am', 'vs more')::text) AS committed;
SELECT dolt_checkout('main');
SELECT dolt_branch('vs_work');
SELECT dolt_checkout('vs_work');

SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('vs_theirs'))[3:4];
SELECT violation_type, violating_row FROM dolt_constraint_violations('vs.child')
ORDER BY 2::text;
-- Set again (checked again), they read the same.
SELECT dolt_verify_constraints('vs.child');
SELECT violation_type, violating_row FROM dolt_constraint_violations('vs.child')
ORDER BY 2::text;
SELECT length(dolt_commit('-afm', 'vs merged')::text) AS committed;
-- Merged again: the child rows' violations, found again, are kept once;
-- the new one (u's 11) follows them.
SELECT (dolt_merge('vs_more'))[3:4];
SELECT split_part(t, '.', 2) AS t, violation_type, violating_row
FROM unnest(ARRAY['vs.child', 'vs.u']) t,
	LATERAL dolt_constraint_violations(t);
SELECT * FROM dolt_constraint_violations() ORDER BY table_name;
SELECT length(dolt_commit('-afm', 'vs merged more')::text) AS committed;

-- Renamed, the branch takes them all, in order; a branch of its old name
-- has none.
SELECT dolt_checkout('main');
SELECT dolt_branch('-m', 'vs_work', 'vs_renamed');
SELECT dolt_branch('vs_work');
SELECT dolt_checkout('vs_work');
SELECT count(*) AS none FROM dolt_constraint_violations();
SELECT dolt_checkout('vs_renamed');
SELECT split_part(t, '.', 2) AS t, violation_type, violating_row
FROM unnest(ARRAY['vs.child', 'vs.u']) t,
	LATERAL dolt_constraint_violations(t);

-- Set again (the rows fixed and checked), those fixed go.
DELETE FROM vs.child;
SELECT dolt_verify_constraints('vs.child');
SELECT split_part(t, '.', 2) AS t, violation_type, violating_row
FROM unnest(ARRAY['vs.child', 'vs.u']) t,
	LATERAL dolt_constraint_violations(t);
SELECT length(dolt_commit('-afm', 'vs checked')::text) AS committed;
-- A branch deleted drops them: one made again of its name has none.
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'vs_renamed');
SELECT dolt_branch('vs_renamed');
SELECT dolt_checkout('vs_renamed');
SELECT count(*) AS none FROM dolt_constraint_violations();
-- A reset drops them.
SELECT dolt_checkout('vs_work');
SELECT (dolt_merge('vs_theirs'))[3:4];
SELECT count(*) AS two FROM dolt_constraint_violations('vs.child');
SELECT dolt_reset('--hard');
SELECT count(*) AS none FROM dolt_constraint_violations();
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'vs_renamed');
SELECT dolt_branch('-D', 'vs_work');
RESET dolt.allow_commit_conflicts;
SELECT dolt_branch('-D', 'vs_theirs');
SELECT dolt_branch('-D', 'vs_more');
DROP SCHEMA vs CASCADE;
SELECT length(dolt_commit('-Am', 'no vs')::text) AS committed;
