-- A move (a reset, a revert, a cherry-pick, a merge) or a branch's first
-- use that would put into the table here a version a commit of another
-- relation holds (a table dropped and made again keeps its name's identity,
-- so the move matches it to the new one) makes the table again instead: a
-- relation of its own, the mover's, with no grants.  The new table's
-- grants, which cover its own rows, don't cover the old table's (#936).
CREATE DATABASE regress_remade_versions;
\c regress_remade_versions
CREATE EXTENSION test_prolly;
CREATE SCHEMA rv VERSIONED;
CREATE ROLE regress_rv_reader;
GRANT USAGE ON SCHEMA rv TO regress_rv_reader;
CREATE TABLE rv.t (id int PRIMARY KEY, note text);
INSERT INTO rv.t VALUES (1, 'old secret');
SELECT length(dolt_commit('-Am', 'old table')::text) AS committed;
SELECT dolt_hashof('HEAD') AS old_commit \gset
-- (a branch of the old table, used before it went)
SELECT dolt_branch('keeps_old') IS NOT NULL AS made;
SELECT dolt_checkout('keeps_old') IS NOT NULL AS on_old;
INSERT INTO rv.t VALUES (2, 'old secret, kept');
SELECT length(dolt_commit('-am', 'kept old')::text) AS committed;
SELECT dolt_hashof('HEAD') AS kept_commit \gset
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
DROP TABLE rv.t;
SELECT length(dolt_commit('-Am', 'dropped')::text) AS committed;
SELECT dolt_hashof('HEAD') AS drop_commit \gset
CREATE TABLE rv.t (id int PRIMARY KEY, note text);
INSERT INTO rv.t VALUES (10, 'public');
GRANT SELECT ON rv.t TO regress_rv_reader;
SELECT length(dolt_commit('-Am', 'new table')::text) AS committed;
SELECT dolt_hashof('HEAD') AS new_commit \gset
SET ROLE regress_rv_reader;
SELECT * FROM rv.t;
RESET ROLE;

-- (each route: the table made again, the mover's, no grants; the reader
-- can't read the old rows, the mover can)
CREATE FUNCTION pg_temp.made_again(rel regclass) RETURNS TABLE (mover bool, grants bool)
LANGUAGE sql AS $$
	SELECT relowner = current_user::regrole, relacl IS NOT NULL
	FROM pg_class WHERE oid = rel
$$;
SELECT 'rv.t'::regclass::oid AS new_oid \gset

-- a hard reset to the old table's commit
SELECT dolt_reset('--hard', :'old_commit') IS NOT NULL AS reset;
SELECT 'rv.t'::regclass::oid <> :new_oid AS another_relation, * FROM pg_temp.made_again('rv.t');
SELECT * FROM rv.t ORDER BY id;
SET ROLE regress_rv_reader;
SELECT * FROM rv.t;
RESET ROLE;
SELECT dolt_reset('--hard', :'new_commit') IS NOT NULL AS back;
GRANT SELECT ON rv.t TO regress_rv_reader;

-- a cherry-pick of the old table's branch's commit
SELECT (dolt_cherry_pick(:'kept_commit'))[1] IS NOT NULL AS picked;
SELECT * FROM pg_temp.made_again('rv.t');
SELECT * FROM rv.t ORDER BY id;
SET ROLE regress_rv_reader;
SELECT * FROM rv.t;
SELECT * FROM dolt_at('HEAD', NULL::rv.t);
RESET ROLE;
SELECT dolt_reset('--hard', :'new_commit') IS NOT NULL AS back;
GRANT SELECT ON rv.t TO regress_rv_reader;

-- a merge of the old table's branch
SELECT (dolt_merge('--no-ff', '-m', 'old in', 'keeps_old'))[3] AS conflicts;
SELECT * FROM pg_temp.made_again('rv.t');
SET ROLE regress_rv_reader;
SELECT * FROM rv.t;
SELECT * FROM dolt_at('HEAD', NULL::rv.t);
RESET ROLE;
SELECT dolt_reset('--hard', :'new_commit') IS NOT NULL AS back;
GRANT SELECT ON rv.t TO regress_rv_reader;

-- a revert of the drop
SELECT (dolt_revert(:'drop_commit'))[1] IS NOT NULL AS reverted;
SELECT * FROM pg_temp.made_again('rv.t');
SET ROLE regress_rv_reader;
SELECT * FROM rv.t;
RESET ROLE;
SELECT dolt_reset('--hard', :'new_commit') IS NOT NULL AS back;
GRANT SELECT ON rv.t TO regress_rv_reader;

-- a branch made at the old table's commit by hash, first used: its copies
-- are of this relation, moved to the old rows, so it makes the table again
-- (as the branch's owner)
SELECT dolt_branch('at_old', :'old_commit') IS NOT NULL AS made;
SELECT dolt_checkout('at_old') IS NOT NULL AS on_at_old;
SELECT * FROM pg_temp.made_again('rv.t');
SELECT * FROM rv.t ORDER BY id;
SET ROLE regress_rv_reader;
SELECT * FROM rv.t;
RESET ROLE;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;

-- a branch made at the old table's commit after the re-make, first used
-- there, with a commit of its own, merged in: its commits are of the
-- relation made again, so neither they nor the old rows read as this
-- table's history
SELECT dolt_branch('late2', :'old_commit') IS NOT NULL AS made;
SELECT dolt_checkout('late2') IS NOT NULL AS on_late2;
SELECT 'rv.t'::regclass::oid <> :new_oid AS another_relation, * FROM pg_temp.made_again('rv.t');
INSERT INTO rv.t VALUES (3, 'late2 row');
SELECT length(dolt_commit('-am', 'late2 row')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('--no-ff', '-m', 'late2 in', 'late2'))[3] AS conflicts;
-- (the merge brought the other relation's rows in: made again here too)
SELECT * FROM pg_temp.made_again('rv.t');
GRANT SELECT ON rv.t TO regress_rv_reader;
SET ROLE regress_rv_reader;
SELECT * FROM dolt_at('HEAD^2', NULL::rv.t);
SELECT * FROM dolt_at('late2', NULL::rv.t);
SELECT * FROM dolt_at(:'old_commit', NULL::rv.t);
RESET ROLE;
SELECT * FROM dolt_at('late2', NULL::rv.t) ORDER BY id;
SELECT dolt_reset('--hard', :'new_commit') IS NOT NULL AS back;
GRANT SELECT ON rv.t TO regress_rv_reader;

-- the same where the copy's definition differs from the old table's, so
-- that the first use makes it again anyway (a column the old table had
-- before a kept one): made again, it is still no copy of this table
CREATE TABLE rv.d (id int PRIMARY KEY, a int, note text);
INSERT INTO rv.d VALUES (1, 1, 'old secret d');
SELECT length(dolt_commit('-Am', 'old d')::text) AS committed;
SELECT dolt_hashof('HEAD') AS old_d \gset
DROP TABLE rv.d;
SELECT length(dolt_commit('-Am', 'dropped d')::text) AS committed;
CREATE TABLE rv.d (id int PRIMARY KEY, note text);
INSERT INTO rv.d VALUES (10, 'public d');
GRANT SELECT ON rv.d TO regress_rv_reader;
SELECT length(dolt_commit('-Am', 'new d')::text) AS committed;
SELECT dolt_hashof('HEAD') AS new_d \gset
SELECT dolt_branch('late_d', :'old_d') IS NOT NULL AS made;
SELECT dolt_checkout('late_d') IS NOT NULL AS on_late_d;
SELECT * FROM pg_temp.made_again('rv.d');
INSERT INTO rv.d VALUES (3, 3, 'late d row');
SELECT length(dolt_commit('-am', 'late d row')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('--no-ff', '-m', 'late_d in', 'late_d'))[3] AS conflicts;
GRANT SELECT ON rv.d TO regress_rv_reader;
SET ROLE regress_rv_reader;
SELECT * FROM dolt_at('HEAD^2', NULL::rv.d);
SELECT * FROM dolt_at('late_d', NULL::rv.d);
RESET ROLE;
SELECT dolt_reset('--hard', :'new_commit') IS NOT NULL AS back;
GRANT SELECT ON rv.t TO regress_rv_reader;

-- a move within the table's own history keeps its relation and grants:
-- the relation made again has its history from then on
INSERT INTO rv.t VALUES (11, 'public, later');
SELECT length(dolt_commit('-am', 'later')::text) AS committed;
SELECT dolt_hashof('HEAD') AS later_commit \gset
SELECT 'rv.t'::regclass::oid AS later_oid \gset
INSERT INTO rv.t VALUES (12, 'public, later still');
SELECT length(dolt_commit('-am', 'later still')::text) AS committed;
SELECT dolt_reset('--hard', :'later_commit') IS NOT NULL AS reset;
SELECT 'rv.t'::regclass::oid = :later_oid AS same_relation;
SET ROLE regress_rv_reader;
SELECT * FROM rv.t ORDER BY id;
RESET ROLE;

\c contrib_regression
DROP DATABASE regress_remade_versions;
DROP ROLE regress_rv_reader;
