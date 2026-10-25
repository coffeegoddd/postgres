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

-- every route that brings a commit's version into the table: a table's
-- checkout from a commit, a rebase, an integration, a stash's pop
CREATE TABLE rv.c (id int PRIMARY KEY, note text);
INSERT INTO rv.c VALUES (1, 'old secret c');
SELECT length(dolt_commit('-Am', 'old c')::text) AS committed;
SELECT dolt_hashof('HEAD') AS old_c \gset
-- (branches of the old table, used before it went)
SELECT dolt_branch('c_rebased') IS NOT NULL AS made;
SELECT dolt_branch('c_integrated') IS NOT NULL AS made;
SELECT dolt_checkout('c_rebased') IS NOT NULL AS on_rebased;
INSERT INTO rv.c VALUES (2, 'old secret c, rebased');
SELECT length(dolt_commit('-am', 'c rebased row')::text) AS committed;
SELECT dolt_checkout('c_integrated') IS NOT NULL AS on_integrated;
INSERT INTO rv.c VALUES (3, 'old secret c, integrated');
SELECT length(dolt_commit('-am', 'c integrated row')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
DROP TABLE rv.c;
SELECT length(dolt_commit('-Am', 'dropped c')::text) AS committed;
CREATE TABLE rv.c (id int PRIMARY KEY, note text);
INSERT INTO rv.c VALUES (10, 'public c');
GRANT SELECT ON rv.c TO regress_rv_reader;
SELECT length(dolt_commit('-Am', 'new c')::text) AS committed;
SELECT dolt_hashof('HEAD') AS new_c \gset
-- (a table checked out of the old commit)
SELECT dolt_checkout(:'old_c', 'rv.c') IS NOT NULL AS checked_out;
SELECT * FROM pg_temp.made_again('rv.c');
SELECT * FROM rv.c ORDER BY id;
SET ROLE regress_rv_reader;
SELECT * FROM rv.c;
RESET ROLE;
SELECT dolt_reset('--hard', :'new_c') IS NOT NULL AS back;
GRANT SELECT ON rv.c TO regress_rv_reader;
SELECT length(dolt_commit('--allow-empty', '-m', 'c granted')::text) AS committed;
SELECT dolt_hashof('HEAD') AS new_c \gset
-- (a rebase of the old table's branch onto main, then main fast-forwarded
-- to it)
SELECT dolt_checkout('c_rebased') IS NOT NULL AS on_rebased;
SELECT dolt_rebase('main') IS NOT NULL AS rebased;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('c_rebased'))[3] AS conflicts;
SELECT * FROM pg_temp.made_again('rv.c');
SET ROLE regress_rv_reader;
SELECT * FROM rv.c;
RESET ROLE;
SELECT dolt_reset('--hard', :'new_c') IS NOT NULL AS back;
GRANT SELECT ON rv.c TO regress_rv_reader;
SELECT length(dolt_commit('--allow-empty', '-m', 'c granted again')::text) AS committed;
-- (the old table's branch integrated into main)
SELECT dolt_checkout('c_integrated') IS NOT NULL AS on_integrated;
SELECT dolt_integrate('main', '-m', 'c integrated') IS NOT NULL AS integrated;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT * FROM pg_temp.made_again('rv.c');
SET ROLE regress_rv_reader;
SELECT * FROM rv.c;
RESET ROLE;
-- (a stash of the old table's rows popped over the new one)
CREATE TABLE rv.s (id int PRIMARY KEY, note text);
SELECT length(dolt_commit('-Am', 'old s')::text) AS committed;
INSERT INTO rv.s VALUES (1, 'old secret s, stashed');
SELECT dolt_stash('push', 's1') IS NOT NULL AS stashed;
DROP TABLE rv.s;
CREATE TABLE rv.s (id int PRIMARY KEY, note text);
INSERT INTO rv.s VALUES (10, 'public s');
GRANT SELECT ON rv.s TO regress_rv_reader;
SELECT length(dolt_commit('-Am', 'new s')::text) AS committed;
SELECT dolt_stash('pop', 's1') IS NOT NULL AS popped;
SELECT * FROM pg_temp.made_again('rv.s');
SET ROLE regress_rv_reader;
SELECT * FROM rv.s;
RESET ROLE;
SELECT dolt_reset('--hard') IS NOT NULL AS discarded;

-- a merge, pick or revert stopped on conflicts: resolving a conflict with
-- the other side's rows brings its version in, and makes the table again
CREATE TABLE rv.k (id int PRIMARY KEY, note text);
INSERT INTO rv.k VALUES (1, 'old secret k');
SELECT length(dolt_commit('-Am', 'old k')::text) AS committed;
SELECT dolt_branch('k_side') IS NOT NULL AS made;
SELECT dolt_checkout('k_side') IS NOT NULL AS on_side;
UPDATE rv.k SET note = 'old secret k, side';
SELECT length(dolt_commit('-am', 'k side')::text) AS committed;
SELECT dolt_hashof('HEAD') AS k_pick \gset
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
UPDATE rv.k SET note = 'old secret k, updated';
SELECT length(dolt_commit('-am', 'k updated')::text) AS committed;
SELECT dolt_hashof('HEAD') AS k_updated \gset
DROP TABLE rv.k;
SELECT length(dolt_commit('-Am', 'dropped k')::text) AS committed;
CREATE TABLE rv.k (id int PRIMARY KEY, note text);
INSERT INTO rv.k VALUES (1, 'public k');
GRANT SELECT ON rv.k TO regress_rv_reader;
SELECT length(dolt_commit('-Am', 'new k')::text) AS committed;
SELECT 'rv.k'::regclass::oid AS k_oid \gset
-- (a merge)
BEGIN;
SELECT (dolt_merge('k_side'))[3] AS conflicts;
SELECT 'rv.k'::regclass::oid = :k_oid AS same_relation;
SELECT dolt_conflicts_resolve('--theirs', 'rv.k') IS NOT NULL AS resolved;
SELECT 'rv.k'::regclass::oid <> :k_oid AS another_relation, * FROM pg_temp.made_again('rv.k');
SELECT length(dolt_commit('-am', 'k merged')::text) AS committed;
ROLLBACK;
-- (a cherry-pick)
BEGIN;
SELECT (dolt_cherry_pick(:'k_pick'))[2] AS conflicts;
SELECT dolt_conflicts_resolve('--theirs', 'rv.k') IS NOT NULL AS resolved;
SELECT 'rv.k'::regclass::oid <> :k_oid AS another_relation, * FROM pg_temp.made_again('rv.k');
ROLLBACK;
-- (a revert)
BEGIN;
SELECT (dolt_revert(:'k_updated'))[2] AS conflicts;
SELECT dolt_conflicts_resolve('--theirs', 'rv.k') IS NOT NULL AS resolved;
SELECT 'rv.k'::regclass::oid <> :k_oid AS another_relation, * FROM pg_temp.made_again('rv.k');
ROLLBACK;

-- a version is another table's by the record's line under its own name: a
-- table dropped and another renamed into its name
CREATE TABLE rv.a (id int PRIMARY KEY, note text);
CREATE TABLE rv.b (id int PRIMARY KEY, note text);
INSERT INTO rv.a VALUES (1, 'a secret');
INSERT INTO rv.b VALUES (2, 'b public');
SELECT length(dolt_commit('-Am', 'a and b')::text) AS committed;
SELECT dolt_hashof('HEAD') AS c_ab \gset
DROP TABLE rv.a;
ALTER TABLE rv.b RENAME TO a;
GRANT SELECT ON rv.a TO regress_rv_reader;
SELECT length(dolt_commit('-Am', 'b renamed to a')::text) AS committed;
SELECT dolt_hashof('HEAD') AS renamed \gset
SELECT 'rv.a'::regclass::oid AS y \gset
SELECT dolt_reset('--hard', :'c_ab') IS NOT NULL AS reset;
SELECT 'rv.a'::regclass::oid <> :y AS another_relation, * FROM pg_temp.made_again('rv.a');
SET ROLE regress_rv_reader;
SELECT * FROM rv.a;
RESET ROLE;
SELECT dolt_reset('--hard', :'renamed') IS NOT NULL AS back;
-- (two tables' names swapped: a reset back puts each version, by its
-- name, into the other's relation, so both are made again)
CREATE TABLE rv.x (id int PRIMARY KEY, note text);
CREATE TABLE rv.y (id int PRIMARY KEY, note text);
INSERT INTO rv.x VALUES (1, 'x secret');
INSERT INTO rv.y VALUES (2, 'y public');
SELECT length(dolt_commit('-Am', 'x and y')::text) AS committed;
SELECT dolt_hashof('HEAD') AS c_xy \gset
SELECT 'rv.x'::regclass::oid AS x_oid, 'rv.y'::regclass::oid AS y_oid \gset
ALTER TABLE rv.x RENAME TO tmp;
ALTER TABLE rv.y RENAME TO x;
ALTER TABLE rv.tmp RENAME TO y;
GRANT SELECT ON rv.x TO regress_rv_reader;
SELECT length(dolt_commit('-Am', 'x and y swapped')::text) AS committed;
SELECT dolt_reset('--hard', :'c_xy') IS NOT NULL AS reset;
SELECT 'rv.x'::regclass::oid NOT IN (:x_oid, :y_oid) AS x_again,
	'rv.y'::regclass::oid NOT IN (:x_oid, :y_oid) AS y_again;
SET ROLE regress_rv_reader;
SELECT * FROM rv.x;
SELECT * FROM rv.y;
RESET ROLE;

-- made again by a reset to the old table's commit, the table is of the
-- old table's class: a later move to the same version (a reset or a
-- stash that discards changes) keeps it, and its grants
CREATE TABLE rv.r (id int PRIMARY KEY, note text);
INSERT INTO rv.r VALUES (1, 'old secret r');
SELECT length(dolt_commit('-Am', 'old r')::text) AS committed;
SELECT dolt_hashof('HEAD') AS old_r \gset
DROP TABLE rv.r;
SELECT length(dolt_commit('-Am', 'dropped r')::text) AS committed;
CREATE TABLE rv.r (id int PRIMARY KEY, note text);
INSERT INTO rv.r VALUES (10, 'public r');
GRANT SELECT ON rv.r TO regress_rv_reader;
SELECT length(dolt_commit('-Am', 'new r')::text) AS committed;
SELECT dolt_hashof('HEAD') AS new_r \gset
SELECT dolt_reset('--hard', :'old_r') IS NOT NULL AS reset;
SELECT 'rv.r'::regclass::oid AS z \gset
GRANT SELECT ON rv.r TO regress_rv_reader;
INSERT INTO rv.r VALUES (5, 'uncommitted');
SELECT dolt_reset('--hard') IS NOT NULL AS discarded;
SELECT 'rv.r'::regclass::oid = :z AS same_relation, * FROM pg_temp.made_again('rv.r');
INSERT INTO rv.r VALUES (6, 'uncommitted');
SELECT dolt_stash('push', 's2') IS NOT NULL AS stashed;
SELECT 'rv.r'::regclass::oid = :z AS same_relation, * FROM pg_temp.made_again('rv.r');
SELECT dolt_stash('drop', 's2') IS NOT NULL AS dropped;
SELECT dolt_reset('--hard', :'new_r') IS NOT NULL AS back;
GRANT SELECT ON rv.r TO regress_rv_reader;

-- a branch's first use makes its copy again as the table's owner, and
-- gives it to the branch's owner: a branch made at the old commit by a
-- role that may make branches but owns no table
CREATE ROLE regress_rv_maker;
SELECT format('GRANT CREATE ON DATABASE %I TO regress_rv_maker', current_database()) \gexec
GRANT USAGE ON SCHEMA rv TO regress_rv_maker;
SET ROLE regress_rv_maker;
SELECT dolt_branch('maker_old', :'old_r') IS NOT NULL AS made;
RESET ROLE;
SELECT dolt_checkout('maker_old') IS NOT NULL AS on_maker_old;
SELECT relowner::regrole AS owner, relacl IS NOT NULL AS grants
FROM pg_class WHERE oid = 'rv.r'::regclass;
SELECT * FROM rv.r;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;

-- a mover that can't make the table again is refused, saying why
CREATE ROLE regress_rv_writer IN ROLE pg_prolly_write;
GRANT USAGE ON SCHEMA rv TO regress_rv_writer;
GRANT SELECT, INSERT, UPDATE, DELETE ON rv.r TO regress_rv_writer;
SET ROLE regress_rv_writer;
SELECT dolt_reset('--hard', :'old_r');
RESET ROLE;
SET ROLE regress_rv_reader;
SELECT * FROM rv.r;
RESET ROLE;

\c contrib_regression
DROP DATABASE regress_remade_versions;
DROP ROLE regress_rv_reader, regress_rv_maker, regress_rv_writer;
