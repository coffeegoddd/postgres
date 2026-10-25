-- A table is its relation (by OID) and its name together (#936).  A table
-- dropped and made again, or one made on each of two branches, keeps its
-- name's identity, but is another relation: another table, whose rows its
-- grants don't cover.  So no move puts one relation's version into another:
-- a merge (a pick, a revert, a rebase's step, an integration, a stash's
-- apply) whose other side brings, under a table's name, another
-- relation's version stops on a table identity conflict, the table kept as
-- the branch has it and no rows merged, until dolt_conflicts_resolve()
-- keeps ours or makes theirs the branch's table; a reset, a table's
-- checkout and a push's branch move refuse; a branch's first use makes
-- the head's relation.  A table a move makes is the relation its version
-- is of, by OID, as a branch's copies are.  Each case is read by a role
-- with SELECT on the table here only.
CREATE DATABASE regress_identity_conflicts;
\c regress_identity_conflicts
CREATE EXTENSION test_prolly;
CREATE SCHEMA ic VERSIONED;
CREATE ROLE regress_ic_reader;
CREATE ROLE regress_ic_old_reader;
CREATE ROLE regress_ic_writer IN ROLE pg_prolly_write;
GRANT USAGE ON SCHEMA ic TO regress_ic_reader, regress_ic_old_reader, regress_ic_writer;
CREATE TABLE ic.t (id int PRIMARY KEY, note text);
INSERT INTO ic.t VALUES (1, 'old secret');
SELECT length(dolt_commit('-Am', 'old table')::text) AS committed;
SELECT dolt_hashof('HEAD') AS old_commit \gset
SELECT 'ic.t'::regclass::oid AS old_oid \gset
-- (branches of the old table, used before it went)
SELECT dolt_branch('keeps_old') IS NOT NULL AS made;
SELECT dolt_branch('rebased_old') IS NOT NULL AS made;
SELECT dolt_branch('integrated_old') IS NOT NULL AS made;
SELECT dolt_checkout('keeps_old') IS NOT NULL AS on_old;
INSERT INTO ic.t VALUES (2, 'old secret, kept');
GRANT SELECT ON ic.t TO regress_ic_old_reader;
SELECT length(dolt_commit('-am', 'kept old')::text) AS committed;
SELECT dolt_hashof('HEAD') AS kept_commit \gset
SELECT dolt_checkout('rebased_old') IS NOT NULL AS on_rebased;
INSERT INTO ic.t VALUES (3, 'old secret, rebased');
SELECT length(dolt_commit('-am', 'rebased old')::text) AS committed;
SELECT dolt_checkout('integrated_old') IS NOT NULL AS on_integrated;
INSERT INTO ic.t VALUES (4, 'old secret, integrated');
SELECT length(dolt_commit('-am', 'integrated old')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
DROP TABLE ic.t;
SELECT length(dolt_commit('-Am', 'dropped')::text) AS committed;
SELECT dolt_hashof('HEAD') AS drop_commit \gset
CREATE TABLE ic.t (id int PRIMARY KEY, note text);
INSERT INTO ic.t VALUES (10, 'public');
GRANT SELECT ON ic.t TO regress_ic_reader;
SELECT length(dolt_commit('-Am', 'new table')::text) AS committed;
SELECT dolt_hashof('HEAD') AS new_commit \gset
SELECT 'ic.t'::regclass::oid AS new_oid \gset
SET ROLE regress_ic_reader;
SELECT * FROM ic.t;
RESET ROLE;

-- a merge of the old table's branch: a table identity conflict, the
-- table kept as the branch has it; listed (ours and theirs, relation and
-- owner), never rows; a commit refused until it is resolved
BEGIN;
SELECT (dolt_merge('keeps_old'))[2:4] AS merged;
SELECT * FROM dolt_status;
SELECT * FROM dolt_conflicts();
SELECT table_name, our_relation = :new_oid AS ours, our_owner = current_user::regrole AS our_owner,
	their_table, their_relation = :old_oid AS theirs,
	their_owner = current_user::regrole AS their_owner, tree
FROM dolt_identity_conflicts();
SET LOCAL ROLE regress_ic_reader;
SELECT * FROM ic.t;
SELECT * FROM dolt_conflicts(NULL::ic.t);
RESET ROLE;
SAVEPOINT s;
SELECT dolt_commit('-m', 'too soon');
ROLLBACK TO SAVEPOINT s;
-- (--ours: the branch's relation, rows and grants kept; the commit, of
-- both parents, records it)
SELECT dolt_conflicts_resolve('--ours', 'ic.t') IS NOT NULL AS resolved;
SELECT * FROM dolt_identity_conflicts();
SELECT length(dolt_commit('-m', 'kept ours')::text) AS committed;
COMMIT;
SELECT 'ic.t'::regclass::oid = :new_oid AS same_relation,
	dolt_hashof('HEAD^2') = :'kept_commit' AS merged_parent;
SET ROLE regress_ic_reader;
SELECT * FROM ic.t;
SELECT * FROM dolt_at('HEAD^2', NULL::ic.t);
RESET ROLE;
SELECT dolt_reset('--hard', :'new_commit') IS NOT NULL AS back;

-- --theirs: the other side's relation (its OID, definition, rows and the
-- grants its branch gives it) made the branch's table, the branch's
-- dropped; it takes owning the branch's table
BEGIN;
SELECT (dolt_merge('keeps_old'))[2:4] AS merged;
GRANT SELECT, INSERT, UPDATE, DELETE ON ic.t TO regress_ic_writer;
SAVEPOINT s;
SET LOCAL ROLE regress_ic_writer;
SELECT dolt_conflicts_resolve('--theirs', 'ic.t');
ROLLBACK TO SAVEPOINT s;
SELECT dolt_conflicts_resolve('--theirs', 'ic.t') IS NOT NULL AS resolved;
SELECT 'ic.t'::regclass::oid = :old_oid AS old_relation,
	has_table_privilege('regress_ic_reader', 'ic.t', 'SELECT') AS reader_granted,
	has_table_privilege('regress_ic_old_reader', 'ic.t', 'SELECT') AS old_reader_granted;
SELECT * FROM ic.t ORDER BY id;
SAVEPOINT s;
SET LOCAL ROLE regress_ic_reader;
SELECT * FROM ic.t;
ROLLBACK TO SAVEPOINT s;
SET LOCAL ROLE regress_ic_old_reader;
SELECT * FROM ic.t ORDER BY id;
RESET ROLE;
SELECT length(dolt_commit('-m', 'took theirs')::text) AS committed;
COMMIT;
-- (the commit records the relation: a later merge of the branch is of the
-- same table)
SELECT dolt_checkout('keeps_old') IS NOT NULL AS on_old;
INSERT INTO ic.t VALUES (5, 'old secret, later');
SELECT length(dolt_commit('-am', 'kept old, later')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('keeps_old'))[2:4] AS merged;
SELECT count(*) AS old_rows FROM ic.t;
-- (back: the reset makes the new table's relation again, by its OID, once
-- the old one is dropped here; its grants went with it)
SELECT dolt_reset('--hard', :'new_commit');
DROP TABLE ic.t;
SELECT dolt_reset('--hard', :'new_commit') IS NOT NULL AS back;
SELECT 'ic.t'::regclass::oid = :new_oid AS new_relation, * FROM ic.t;
GRANT SELECT ON ic.t TO regress_ic_reader;
SELECT length(dolt_commit('--allow-empty', '-m', 'granted')::text) AS committed;
SELECT dolt_hashof('HEAD') AS new_commit \gset

-- an abort after --theirs makes the branch's relation again (its grants
-- gone with it)
BEGIN;
SELECT (dolt_merge('keeps_old'))[2:4] AS merged;
SELECT dolt_conflicts_resolve('--theirs', 'ic.t') IS NOT NULL AS resolved;
SELECT (dolt_merge('--abort'))[4] AS aborted;
SELECT 'ic.t'::regclass::oid = :new_oid AS new_relation, * FROM ic.t;
ROLLBACK;

-- a merge stopped and kept (dolt.allow_commit_conflicts): durable, shown
-- to other sessions, and a commit refused until it is resolved; an abort
-- clears it.  Without the setting, outside a transaction block, the merge
-- is rolled back.
SELECT dolt_merge('keeps_old');
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('keeps_old'))[2:4] AS merged;
RESET dolt.allow_commit_conflicts;
\c regress_identity_conflicts
SELECT * FROM dolt_status;
SELECT table_name, tree FROM dolt_identity_conflicts();
SELECT dolt_commit('-m', 'too soon');
SET ROLE regress_ic_reader;
SELECT * FROM ic.t;
RESET ROLE;
SELECT (dolt_merge('--abort'))[4] AS aborted;
SELECT count(*) AS conflicts FROM dolt_identity_conflicts();
SELECT count(*) AS changed FROM dolt_status;

-- a cherry-pick of the old table's branch's commit, and a revert of the
-- drop: stopped the same, the reader reading the table here only
BEGIN;
SELECT (dolt_cherry_pick(:'kept_commit'))[2:4] AS conflicts;
SELECT table_name, their_relation = :old_oid AS theirs FROM dolt_identity_conflicts();
SET LOCAL ROLE regress_ic_reader;
SELECT * FROM ic.t;
SELECT * FROM dolt_conflicts(NULL::ic.t);
RESET ROLE;
SELECT dolt_conflicts_resolve('--theirs', 'ic.t') IS NOT NULL AS resolved;
SELECT 'ic.t'::regclass::oid = :old_oid AS old_relation, * FROM ic.t ORDER BY id;
ROLLBACK;
BEGIN;
SELECT (dolt_revert(:'drop_commit'))[2:4] AS conflicts;
SELECT table_name, their_relation = :old_oid AS theirs FROM dolt_identity_conflicts();
SET LOCAL ROLE regress_ic_reader;
SELECT * FROM ic.t;
RESET ROLE;
SELECT dolt_conflicts_resolve('--ours', 'ic.t') IS NOT NULL AS resolved;
SELECT (dolt_revert('--continue'))[1] IS NOT NULL AS reverted;
SELECT 'ic.t'::regclass::oid = :new_oid AS new_relation, * FROM ic.t;
ROLLBACK;

-- a rebase of the old table's branch onto main stops at its step the same;
-- resolved, it goes on, and the branch is the rebased commits, their
-- relations
SELECT dolt_checkout('rebased_old') IS NOT NULL AS on_rebased;
BEGIN;
SELECT (dolt_rebase('main'))[1] AS stopped;
SELECT table_name FROM dolt_identity_conflicts();
SET LOCAL ROLE regress_ic_reader;
SELECT * FROM ic.t;
RESET ROLE;
SELECT dolt_conflicts_resolve('--ours', 'ic.t') IS NOT NULL AS resolved;
SELECT (dolt_rebase('--continue'))[1] AS status;
COMMIT;
SELECT active_branch(), 'ic.t'::regclass::oid = :new_oid AS new_relation;
SELECT * FROM ic.t;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;

-- an integration of the old table's branch: it fails on conflicts
SELECT dolt_checkout('integrated_old') IS NOT NULL AS on_integrated;
SELECT dolt_integrate('main', '-m', 'integrated');
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SET ROLE regress_ic_reader;
SELECT * FROM ic.t;
RESET ROLE;

-- a stash's apply has no merge in progress to keep it in: refused
CREATE TABLE ic.s (id int PRIMARY KEY, note text);
SELECT length(dolt_commit('-Am', 'old s')::text) AS committed;
INSERT INTO ic.s VALUES (1, 'old secret s, stashed');
SELECT dolt_stash('push', 's1') IS NOT NULL AS stashed;
DROP TABLE ic.s;
CREATE TABLE ic.s (id int PRIMARY KEY, note text);
INSERT INTO ic.s VALUES (10, 'public s');
GRANT SELECT ON ic.s TO regress_ic_reader;
SELECT length(dolt_commit('-Am', 'new s')::text) AS committed;
SELECT dolt_stash('pop', 's1');
SET ROLE regress_ic_reader;
SELECT * FROM ic.s;
RESET ROLE;
-- (a stash's push over a table made again, uncommitted, leaves the head's
-- relation, made again by its OID; the stash holds the other)
SELECT 'ic.s'::regclass::oid AS s_oid \gset
DROP TABLE ic.s;
CREATE TABLE ic.s (id int PRIMARY KEY, note text);
INSERT INTO ic.s VALUES (20, 'stashed s');
SELECT dolt_stash('push', 's2') IS NOT NULL AS stashed;
SELECT 'ic.s'::regclass::oid = :s_oid AS head_relation, * FROM ic.s;
SELECT dolt_stash('drop', 's2') IS NOT NULL AS dropped;
SELECT dolt_stash('drop', 's1') IS NOT NULL AS dropped;

-- a table made on both sides apart is two tables: the same conflict
SELECT dolt_branch('p_side') IS NOT NULL AS made;
CREATE TABLE ic.p (id int PRIMARY KEY, note text);
INSERT INTO ic.p VALUES (1, 'main p');
GRANT SELECT ON ic.p TO regress_ic_reader;
SELECT length(dolt_commit('-Am', 'main p')::text) AS committed;
SELECT dolt_checkout('p_side') IS NOT NULL AS on_side;
CREATE TABLE ic.p (id int PRIMARY KEY, note text);
INSERT INTO ic.p VALUES (2, 'side p secret');
SELECT length(dolt_commit('-Am', 'side p')::text) AS committed;
SELECT 'ic.p'::regclass::oid AS side_p \gset
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
BEGIN;
SELECT (dolt_merge('p_side'))[2:4] AS merged;
SELECT table_name FROM dolt_identity_conflicts();
SET LOCAL ROLE regress_ic_reader;
SELECT * FROM ic.p;
RESET ROLE;
SELECT dolt_conflicts_resolve('--theirs', 'ic.p') IS NOT NULL AS resolved;
SELECT 'ic.p'::regclass::oid = :side_p AS side_relation, * FROM ic.p;
SELECT length(dolt_commit('-m', 'side p in')::text) AS committed;
COMMIT;

-- a table dropped and made again on a branch: merged back, no
-- fast-forward, the same conflict
SELECT dolt_branch('remade') IS NOT NULL AS made;
SELECT dolt_checkout('remade') IS NOT NULL AS on_remade;
DROP TABLE ic.t;
CREATE TABLE ic.t (id int PRIMARY KEY, note text);
INSERT INTO ic.t VALUES (30, 'remade secret');
SELECT length(dolt_commit('-Am', 'remade t')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
BEGIN;
SELECT (dolt_merge('remade'))[2:4] AS merged;
SELECT table_name FROM dolt_identity_conflicts();
SET LOCAL ROLE regress_ic_reader;
SELECT * FROM ic.t;
RESET ROLE;
ROLLBACK;

-- a table made on a branch and merged in is the branch's relation, by its
-- OID: merged again, it is the same table, with no conflict
SELECT dolt_branch('f_side') IS NOT NULL AS made;
SELECT dolt_checkout('f_side') IS NOT NULL AS on_side;
CREATE TABLE ic.f (id int PRIMARY KEY);
INSERT INTO ic.f VALUES (1);
SELECT length(dolt_commit('-Am', 'f')::text) AS committed;
SELECT 'ic.f'::regclass::oid AS f_oid \gset
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
INSERT INTO ic.t VALUES (11, 'public, main');
SELECT length(dolt_commit('-am', 'main meanwhile')::text) AS committed;
SELECT (dolt_merge('f_side'))[2:4] AS merged;
SELECT 'ic.f'::regclass::oid = :f_oid AS same_relation;
SELECT dolt_checkout('f_side') IS NOT NULL AS on_side;
INSERT INTO ic.f VALUES (2);
SELECT length(dolt_commit('-am', 'f again')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('f_side'))[2:4] AS merged;
SELECT * FROM ic.f ORDER BY id;

-- a table dropped and another renamed into its name is another table: a
-- reset, or a table's checkout, to a commit before is refused
CREATE TABLE ic.a (id int PRIMARY KEY, note text);
CREATE TABLE ic.b (id int PRIMARY KEY, note text);
INSERT INTO ic.a VALUES (1, 'a secret');
INSERT INTO ic.b VALUES (2, 'b public');
SELECT length(dolt_commit('-Am', 'a and b')::text) AS committed;
SELECT dolt_hashof('HEAD') AS c_ab \gset
DROP TABLE ic.a;
ALTER TABLE ic.b RENAME TO a;
GRANT SELECT ON ic.a TO regress_ic_reader;
SELECT length(dolt_commit('-Am', 'b renamed to a')::text) AS committed;
SELECT dolt_reset('--hard', :'c_ab');
SELECT dolt_checkout(:'c_ab', 'ic.a');
SET ROLE regress_ic_reader;
SELECT * FROM ic.a;
RESET ROLE;

-- the reset and table checkout routes, with the new table: refused, the
-- table as it was; dropped here first, the move makes the old relation
-- (no grants)
SELECT dolt_reset('--hard', :'old_commit');
SELECT dolt_checkout(:'old_commit', 'ic.t');
SET ROLE regress_ic_reader;
SELECT * FROM ic.t ORDER BY id;
RESET ROLE;
SELECT dolt_hashof('HEAD') AS before_drop \gset
DROP TABLE ic.t;
SELECT dolt_checkout(:'old_commit', 'ic.t') IS NOT NULL AS checked_out;
SELECT 'ic.t'::regclass::oid = :old_oid AS old_relation,
	has_table_privilege('regress_ic_reader', 'ic.t', 'SELECT') AS reader_granted;
SELECT * FROM ic.t;
DROP TABLE ic.t;
SELECT dolt_reset('--hard', :'before_drop') IS NOT NULL AS back;

-- an inheritance tree: a child made again on one side keeps the whole tree
-- as the branch has it; --theirs takes the other side's tree
CREATE TABLE ic.ip (id int PRIMARY KEY, note text);
CREATE TABLE ic.ic (extra int) INHERITS (ic.ip);
INSERT INTO ic.ic VALUES (1, 'child base', 0);
SELECT length(dolt_commit('-Am', 'inheritance')::text) AS committed;
SELECT dolt_branch('i_side') IS NOT NULL AS made;
DROP TABLE ic.ic;
CREATE TABLE ic.ic (extra int) INHERITS (ic.ip);
INSERT INTO ic.ic VALUES (2, 'main child', 0);
GRANT SELECT ON ic.ip TO regress_ic_reader;
SELECT length(dolt_commit('-Am', 'child again')::text) AS committed;
SELECT dolt_checkout('i_side') IS NOT NULL AS on_side;
UPDATE ic.ic SET note = 'side child secret';
SELECT length(dolt_commit('-am', 'side child')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
BEGIN;
SELECT (dolt_merge('i_side'))[2:4] AS merged;
SELECT table_name, tree FROM dolt_identity_conflicts() ORDER BY table_name;
SET LOCAL ROLE regress_ic_reader;
SELECT * FROM ic.ip;
RESET ROLE;
SELECT dolt_conflicts_resolve('--theirs', 'ic.ip') IS NOT NULL AS resolved;
SELECT * FROM ic.ip;
SELECT count(*) AS conflicts FROM dolt_identity_conflicts();
ROLLBACK;

-- a partition tree: a partition made again likewise; and a reset to the
-- tree's earlier commit is refused (the partition, not the partitioned
-- table, holds the rows)
CREATE TABLE ic.pt (id int PRIMARY KEY, note text) PARTITION BY RANGE (id);
CREATE TABLE ic.pt1 PARTITION OF ic.pt FOR VALUES FROM (0) TO (100);
CREATE TABLE ic.pt2 PARTITION OF ic.pt FOR VALUES FROM (100) TO (200);
INSERT INTO ic.pt VALUES (1, 'pt1 base'), (101, 'pt2 base');
SELECT length(dolt_commit('-Am', 'partitions')::text) AS committed;
SELECT dolt_hashof('HEAD') AS pt_commit \gset
SELECT dolt_branch('pt_side') IS NOT NULL AS made;
DROP TABLE ic.pt1;
CREATE TABLE ic.pt1 PARTITION OF ic.pt FOR VALUES FROM (0) TO (100);
INSERT INTO ic.pt VALUES (5, 'main pt1');
GRANT SELECT ON ic.pt TO regress_ic_reader;
SELECT length(dolt_commit('-Am', 'pt1 again')::text) AS committed;
SELECT dolt_hashof('HEAD') AS pt_again \gset
SELECT dolt_reset('--hard', :'pt_commit');
SET ROLE regress_ic_reader;
SELECT * FROM ic.pt ORDER BY id;
RESET ROLE;
SELECT dolt_checkout('pt_side') IS NOT NULL AS on_side;
UPDATE ic.pt SET note = 'side secret';
SELECT length(dolt_commit('-am', 'side pt')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
BEGIN;
SELECT (dolt_merge('pt_side'))[2:4] AS merged;
SELECT table_name, our_relation IS NOT NULL AS ours,
	their_relation IS NOT NULL AS theirs, tree
FROM dolt_identity_conflicts() ORDER BY table_name;
SET LOCAL ROLE regress_ic_reader;
SELECT * FROM ic.pt ORDER BY id;
RESET ROLE;
SELECT dolt_conflicts_resolve('--theirs', 'ic.pt2') IS NOT NULL AS resolved;
SELECT * FROM ic.pt ORDER BY id;
ROLLBACK;

-- --theirs refuses where dropping the table here would take with it what
-- the move can't make again as the merge has it (a materialized view,
-- whose query would run as the resolver); a view is made again
CREATE VIEW ic.tv AS SELECT id FROM ic.t;
CREATE MATERIALIZED VIEW ic.tm AS SELECT id FROM ic.t;
SELECT length(dolt_commit('-Am', 'views')::text) AS committed;
BEGIN;
SELECT (dolt_merge('keeps_old'))[2:4] AS merged;
SAVEPOINT s;
SELECT dolt_conflicts_resolve('--theirs', 'ic.t');
ROLLBACK TO SAVEPOINT s;
DROP MATERIALIZED VIEW ic.tm;
SELECT dolt_conflicts_resolve('--theirs', 'ic.t') IS NOT NULL AS resolved;
SELECT * FROM ic.tv ORDER BY id;
ROLLBACK;

-- a branch first used after its table was made again on the branch it
-- copies: its copy is another relation than its head's, so the first use
-- makes the head's (no grants), not the copy's
SELECT dolt_branch('at_old', :'old_commit') IS NOT NULL AS made;
SELECT dolt_branch('-D', 'keeps_old', 'rebased_old', 'integrated_old') IS NOT NULL AS deleted;
SELECT dolt_checkout('at_old') IS NOT NULL AS on_at_old;
SELECT 'ic.t'::regclass::oid = :old_oid AS old_relation,
	has_table_privilege('regress_ic_reader', 'ic.t', 'SELECT') AS reader_granted;
SELECT * FROM ic.t;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;

-- a push's move of a branch to a commit whose table is another relation
-- than the branch's: refused, as a hard reset is
CREATE FUNCTION pg_temp.h(ref text) RETURNS bytea LANGUAGE sql
  AS $$ SELECT decode(dolt_hashof(ref), 'hex') $$;
SELECT value AS fmt FROM test_fold_items() WHERE key = '!:format' \gset
SELECT dolt_branch('pushed') IS NOT NULL AS made;
SELECT dolt_checkout('pushed') IS NOT NULL AS on_pushed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_serve_move_head('pushed', pg_temp.h('pushed'), pg_temp.h(:'old_commit'), :'fmt', false);
SELECT dolt_hashof('pushed') = dolt_hashof('main') AS not_moved;

-- a table a move makes from a commit (a branch's first use, a rebase's
-- end, a reset or table checkout) is its recorded owner's, with no grants:
-- the role moving gains nothing, and one that owns no table and reads none
-- can't take a dropped table's old relation by using a branch at its
-- commit
DROP VIEW ic.tv;
DROP MATERIALIZED VIEW ic.tm;
SELECT length(dolt_commit('-Am', 'no views')::text) AS committed;
CREATE ROLE regress_ic_owner;
GRANT USAGE, CREATE ON SCHEMA ic TO regress_ic_owner;
GRANT CREATE ON SCHEMA ic TO regress_ic_writer;
SELECT format('GRANT CREATE ON DATABASE %I TO regress_ic_writer', current_database()) \gexec
SET ROLE regress_ic_owner;
CREATE TABLE ic.o (id int PRIMARY KEY, note text);
INSERT INTO ic.o VALUES (1, 'owner secret');
RESET ROLE;
SELECT length(dolt_commit('-Am', 'o')::text) AS committed;
SELECT dolt_hashof('HEAD') AS o_commit \gset
SELECT 'ic.o'::regclass::oid AS o_oid \gset
SET ROLE regress_ic_owner;
DROP TABLE ic.o;
CREATE TABLE ic.o (id int PRIMARY KEY, note text);
INSERT INTO ic.o VALUES (10, 'owner public');
GRANT SELECT, INSERT, UPDATE, DELETE ON ic.o TO regress_ic_writer;
RESET ROLE;
SELECT length(dolt_commit('-Am', 'o again')::text) AS committed;
SET ROLE regress_ic_writer;
SELECT dolt_branch('o_old', :'o_commit') IS NOT NULL AS made;
SELECT dolt_checkout('o_old') IS NOT NULL AS on_o_old;
SELECT 'ic.o'::regclass::oid = :o_oid AS old_relation,
	(SELECT relowner::regrole FROM pg_class WHERE oid = 'ic.o'::regclass) AS owner,
	has_table_privilege('ic.o', 'SELECT') AS can_read;
SELECT * FROM ic.o;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
RESET ROLE;
-- (a reset making a table dropped since makes it so too)
SELECT dolt_hashof('HEAD') AS o_now \gset
DROP TABLE ic.o;
SELECT dolt_reset('--hard', :'o_commit') IS NOT NULL AS reset;
SELECT (SELECT relowner::regrole FROM pg_class WHERE oid = 'ic.o'::regclass) AS owner,
	(SELECT relacl FROM pg_class WHERE oid = 'ic.o'::regclass) IS NULL AS no_grants;
DROP TABLE ic.o;
SELECT dolt_reset('--hard', :'o_now') IS NOT NULL AS back;
-- (made so too, its grants given again)
GRANT SELECT, INSERT, UPDATE, DELETE ON ic.o TO regress_ic_writer;
SELECT length(dolt_commit('--allow-empty', '-m', 'o granted')::text) AS committed;

-- a rebase's end: the rebased commits are the branch's; resolved with
-- --ours (the upstream's relation) the table is its recorded owner's, not
-- the rebaser's; with --theirs, the branch's own relation, its owner and
-- grants, stays
SELECT dolt_branch('feat') IS NOT NULL AS made;
SELECT dolt_branch('feat2') IS NOT NULL AS made;
-- (each used, its copies made, before main makes its table again)
SELECT dolt_checkout('feat') IS NOT NULL AS on_feat;
SELECT dolt_checkout('feat2') IS NOT NULL AS on_feat2;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SET ROLE regress_ic_owner;
DROP TABLE ic.o;
CREATE TABLE ic.o (id int PRIMARY KEY, note text);
INSERT INTO ic.o VALUES (20, 'main, made again');
GRANT INSERT, UPDATE, DELETE ON ic.o TO regress_ic_writer;
RESET ROLE;
SELECT length(dolt_commit('-Am', 'o made again on main')::text) AS committed;
SELECT 'ic.o'::regclass::oid AS main_o \gset
SET ROLE regress_ic_writer;
SELECT dolt_checkout('feat') IS NOT NULL AS on_feat;
SELECT 'ic.o'::regclass::oid AS feat_o \gset
INSERT INTO ic.o VALUES (3, 'feat');
SELECT length(dolt_commit('-am', 'f1')::text) AS committed;
BEGIN;
SELECT (dolt_rebase('main'))[1] AS stopped;
SELECT dolt_conflicts_resolve('--ours', 'ic.o') IS NOT NULL AS resolved;
SELECT (dolt_rebase('--continue'))[1] AS status;
COMMIT;
SELECT 'ic.o'::regclass::oid = :main_o AS upstream_relation,
	(SELECT relowner::regrole FROM pg_class WHERE oid = 'ic.o'::regclass) AS owner,
	has_table_privilege('ic.o', 'SELECT') AS can_read;
SELECT * FROM ic.o;
SELECT dolt_checkout('feat2') IS NOT NULL AS on_feat2;
SELECT 'ic.o'::regclass::oid AS feat2_o \gset
INSERT INTO ic.o VALUES (4, 'feat2');
SELECT length(dolt_commit('-am', 'f2')::text) AS committed;
RESET ROLE;
BEGIN;
SELECT (dolt_rebase('main'))[1] AS stopped;
SELECT dolt_conflicts_resolve('--theirs', 'ic.o') IS NOT NULL AS resolved;
SELECT (dolt_rebase('--continue'))[1] AS status;
COMMIT;
SELECT 'ic.o'::regclass::oid = :feat2_o AS own_relation,
	(SELECT relowner::regrole FROM pg_class WHERE oid = 'ic.o'::regclass) AS owner,
	has_table_privilege('regress_ic_writer', 'ic.o', 'SELECT') AS writer_reads;
SELECT * FROM ic.o ORDER BY id;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;

-- an abort after --theirs gives the branch's relation back as it was: its
-- grants and its columns', and its views' (made again by --theirs, and by
-- the abort, they get theirs back)
CREATE TABLE ic.g (id int PRIMARY KEY, note text);
CREATE VIEW ic.gv AS SELECT id FROM ic.g;
INSERT INTO ic.g VALUES (1, 'g base');
SELECT length(dolt_commit('-Am', 'g')::text) AS committed;
SELECT dolt_branch('g_side') IS NOT NULL AS made;
DROP TABLE ic.g CASCADE;
CREATE TABLE ic.g (id int PRIMARY KEY, note text);
CREATE VIEW ic.gv AS SELECT id FROM ic.g;
INSERT INTO ic.g VALUES (10, 'g public');
GRANT SELECT ON ic.g TO regress_ic_reader;
GRANT UPDATE (note) ON ic.g TO regress_ic_writer;
GRANT SELECT ON ic.gv TO regress_ic_reader;
SELECT length(dolt_commit('-Am', 'g again')::text) AS committed;
SELECT 'ic.g'::regclass::oid AS g_oid \gset
SELECT dolt_checkout('g_side') IS NOT NULL AS on_side;
UPDATE ic.g SET note = 'g side secret';
SELECT length(dolt_commit('-am', 'g side')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
CREATE TEMP TABLE g_was AS SELECT 'ic.g'::regclass::oid AS relid;
CREATE FUNCTION pg_temp.g_grants() RETURNS TABLE (g_relation bool, g aclitem[], g_note aclitem[], gv aclitem[])
LANGUAGE sql AS $$
	SELECT 'ic.g'::regclass::oid = (SELECT relid FROM g_was),
		(SELECT relacl FROM pg_class WHERE oid = 'ic.g'::regclass),
		(SELECT attacl FROM pg_attribute WHERE attrelid = 'ic.g'::regclass AND attname = 'note'),
		(SELECT relacl FROM pg_class WHERE oid = 'ic.gv'::regclass)
$$;
SELECT * FROM pg_temp.g_grants();
BEGIN;
SELECT (dolt_merge('g_side'))[2:4] AS merged;
SELECT dolt_conflicts_resolve('--theirs', 'ic.g') IS NOT NULL AS resolved;
SELECT * FROM pg_temp.g_grants();
SELECT (dolt_merge('--abort'))[4] AS aborted;
SELECT * FROM pg_temp.g_grants();
COMMIT;
SET ROLE regress_ic_reader;
SELECT * FROM ic.gv;
RESET ROLE;

-- --theirs refuses while objects of other roles depend on the table here:
-- made again, they would be the resolver's
CREATE ROLE regress_ic_viewer;
GRANT USAGE, CREATE ON SCHEMA ic TO regress_ic_viewer;
GRANT SELECT ON ic.g TO regress_ic_viewer;
CREATE TABLE ic.h (id int PRIMARY KEY);
GRANT SELECT ON ic.h TO regress_ic_viewer;
SET ROLE regress_ic_viewer;
CREATE VIEW ic.hv AS SELECT id FROM ic.h;
RESET ROLE;
SELECT length(dolt_commit('-Am', 'h')::text) AS committed;
SELECT dolt_branch('h_side') IS NOT NULL AS made;
DROP TABLE ic.h CASCADE;
CREATE TABLE ic.h (id int PRIMARY KEY);
GRANT SELECT ON ic.h TO regress_ic_viewer;
SET ROLE regress_ic_viewer;
CREATE VIEW ic.hv AS SELECT id FROM ic.h;
RESET ROLE;
SELECT length(dolt_commit('--allow-empty', '-Am', 'h again')::text) AS committed;
SELECT dolt_checkout('h_side') IS NOT NULL AS on_side;
INSERT INTO ic.h VALUES (1);
SELECT length(dolt_commit('-am', 'h side')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
BEGIN;
SELECT (dolt_merge('h_side'))[2:4] AS merged;
SELECT dolt_conflicts_resolve('--theirs', 'ic.h');
ROLLBACK;

-- a branch first used, its source having a materialized view over the
-- table made again: the copy, and the view, are another branch's; the
-- first use makes the head's
SELECT dolt_branch('mv_old') IS NOT NULL AS made;
DROP VIEW ic.gv;
DROP TABLE ic.g;
CREATE TABLE ic.g (id int PRIMARY KEY, note text);
CREATE MATERIALIZED VIEW ic.gm AS SELECT id FROM ic.g;
SELECT length(dolt_commit('-Am', 'g with a materialized view')::text) AS committed;
SELECT dolt_checkout('mv_old') IS NOT NULL AS on_mv_old;
SELECT 'ic.g'::regclass::oid = :g_oid AS head_relation, to_regclass('ic.gm') IS NULL AS no_matview;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;

-- a hard reset to the head discards the branch's own uncommitted drop and
-- re-make: the head's relation again
SELECT 'ic.g'::regclass::oid AS g_now \gset
DROP MATERIALIZED VIEW ic.gm;
DROP TABLE ic.g;
CREATE TABLE ic.g (id int PRIMARY KEY, note text);
SELECT dolt_reset('--hard') IS NOT NULL AS reset;
SELECT 'ic.g'::regclass::oid = :g_now AS head_relation, to_regclass('ic.gm') IS NOT NULL AS matview_back;

-- a forced move of a branch to a commit whose table is another relation
-- than the branch's: refused, the branch as it was
SELECT dolt_branch('forced') IS NOT NULL AS made;
SELECT dolt_checkout('forced') IS NOT NULL AS on_forced;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_hashof('forced') AS forced_head \gset
SELECT dolt_branch('-f', 'forced', :'old_commit');
SELECT dolt_hashof('forced') = :'forced_head' AS unchanged;

-- a table whose recorded owner was dropped since can't be made as the
-- commit has it
CREATE ROLE regress_ic_gone;
GRANT USAGE, CREATE ON SCHEMA ic TO regress_ic_gone;
SET ROLE regress_ic_gone;
CREATE TABLE ic.gone (id int PRIMARY KEY);
RESET ROLE;
SELECT length(dolt_commit('-Am', 'gone')::text) AS committed;
SELECT dolt_hashof('HEAD') AS gone_commit \gset
DROP TABLE ic.gone;
SELECT length(dolt_commit('-Am', 'gone dropped')::text) AS committed;
REVOKE USAGE, CREATE ON SCHEMA ic FROM regress_ic_gone;
DROP ROLE regress_ic_gone;
SELECT dolt_reset('--hard', :'gone_commit');
SELECT to_regclass('ic.gone') IS NULL AS not_made;

\c contrib_regression
DROP DATABASE regress_identity_conflicts;
DROP ROLE regress_ic_reader, regress_ic_old_reader, regress_ic_writer,
	regress_ic_owner, regress_ic_viewer;
