-- The branches, tags and stashes a role makes are shared dependencies of
-- the role, as the objects it owns are: DROP ROLE refuses a role that still
-- makes any and names them, REASSIGN OWNED gives them to another role, and
-- DROP OWNED deletes its tags and stashes and gives its branches to the
-- database's owner (#931).
CREATE DATABASE regress_role_makers;
\c regress_role_makers
CREATE EXTENSION test_prolly;
CREATE SCHEMA rm VERSIONED;
CREATE TABLE rm.t (a int PRIMARY KEY);
GRANT USAGE ON SCHEMA rm TO PUBLIC;
GRANT ALL ON rm.t TO PUBLIC;
INSERT INTO rm.t VALUES (1);
SELECT length(dolt_commit('-Am', 'one')::text) AS committed;
CREATE ROLE regress_rm_maker;
CREATE ROLE regress_rm_heir;
CREATE ROLE regress_rm_other;

-- the role's objects, and what each records
CREATE VIEW made AS
SELECT pg_describe_object(classid, objid, objsubid) AS object,
	refobjid::regrole AS role, deptype
FROM pg_shdepend
WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database())
	-- (the kinds' class ids: a branch, a tag, a stash)
	AND classid IN (9907, 9908, 9909);
GRANT CREATE ON DATABASE regress_role_makers TO regress_rm_maker;
SET ROLE regress_rm_maker;
SELECT dolt_branch('made');
SELECT dolt_branch('renamed_from');
SELECT dolt_branch('deleted');
SELECT dolt_tag('made_tag');
SELECT dolt_tag('deleted_tag');
INSERT INTO rm.t VALUES (2);
SELECT dolt_stash('push', 'shelf');
INSERT INTO rm.t VALUES (3);
SELECT dolt_stash('push', 'shelf');
RESET ROLE;
REVOKE CREATE ON DATABASE regress_role_makers FROM regress_rm_maker;
SELECT * FROM made ORDER BY object;

-- a rename keeps the dependency, and a deletion drops it, as does a stash
-- dropped or popped
SELECT dolt_branch('-m', 'renamed_from', 'renamed');
SELECT dolt_branch('-D', 'deleted');
SELECT dolt_tag('-d', 'deleted_tag');
SELECT dolt_stash('drop', 'shelf', 'stash@{0}');
SELECT * FROM made ORDER BY object;
SELECT name, creator FROM dolt_branches WHERE name <> 'main' ORDER BY name;

-- DROP ROLE refuses the role, naming what it makes
DROP ROLE regress_rm_maker;

-- REASSIGN OWNED gives them to the new role, which then owns them
REASSIGN OWNED BY regress_rm_maker TO regress_rm_heir;
SELECT * FROM made ORDER BY object;
SELECT name, creator FROM dolt_branches WHERE name <> 'main' ORDER BY name;
DROP ROLE regress_rm_maker;
SET ROLE regress_rm_other;
SELECT dolt_branch('-f', 'renamed', 'main');
RESET ROLE;
SET ROLE regress_rm_heir;
SELECT dolt_branch('-f', 'renamed', 'main');
RESET ROLE;

-- a forced move of a branch without commits keeps its maker
SELECT dolt_branch('-f', 'made', 'main');
SELECT * FROM made WHERE object LIKE 'branch "made"';

-- DROP OWNED deletes the role's tags and stashes and gives its branches to
-- the database's owner: deleting a branch would lose its commits
DROP OWNED BY regress_rm_heir;
SELECT * FROM made ORDER BY object;
SELECT name, creator = current_user AS database_owners FROM dolt_branches
WHERE name <> 'main' ORDER BY name;
SELECT count(*) AS tags FROM dolt_tags;
SELECT count(*) AS stashes FROM dolt_stashes;
DROP ROLE regress_rm_heir;

-- a stash the role pushed alone, dropped with the stash's list's others
SET ROLE regress_rm_other;
INSERT INTO rm.t VALUES (4);
SELECT dolt_stash('push', 'shelf');
RESET ROLE;
INSERT INTO rm.t VALUES (5);
SELECT dolt_stash('push', 'shelf');
SELECT * FROM made ORDER BY object;
DROP OWNED BY regress_rm_other;
SELECT * FROM made ORDER BY object;
SELECT name, stash_id FROM dolt_stashes;
-- a clear drops each stash's dependency, so the role may then be dropped
SET ROLE regress_rm_other;
INSERT INTO rm.t VALUES (4);
SELECT dolt_stash('push', 'shelf');
RESET ROLE;
SELECT * FROM made ORDER BY object;
SELECT dolt_stash('clear', 'shelf');
SELECT * FROM made ORDER BY object;
DROP ROLE regress_rm_other;

-- a list of several stashes keeps each one's commit through a collection
INSERT INTO rm.t VALUES (6);
SELECT dolt_stash('push', 'many');
INSERT INTO rm.t VALUES (7);
SELECT dolt_stash('push', 'many');
INSERT INTO rm.t VALUES (8);
SELECT dolt_stash('push', 'many');
SELECT dolt_gc('--full') IS NOT NULL AS collected;
SELECT dolt_stash('pop', 'many', 'stash@{2}');
SELECT a FROM rm.t ORDER BY a;
SELECT dolt_reset('--hard');
SELECT dolt_stash('pop', 'many', 'stash@{1}');
SELECT a FROM rm.t ORDER BY a;
SELECT dolt_reset('--hard');
SELECT dolt_stash('clear', 'many');

\c contrib_regression
DROP DATABASE regress_role_makers;
