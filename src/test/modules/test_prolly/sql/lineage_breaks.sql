-- A history read by a role that doesn't read every version takes a
-- version for the table here only along commits made from the table's
-- relation, as each commit this database makes records them.  Commits name
-- tables by name and identity alone, and a table dropped and made again
-- keeps its name's identity, so the names can't tell it from the old one
-- wherever a commit holds the new table above one holding the old: a head
-- moved to the old table's commit and then committed, a merge of a branch
-- that still has it, a squash of the drop into the re-make, a branch made
-- at the old table's commit.  The records can (#862).
CREATE DATABASE regress_lineage_breaks;
\c regress_lineage_breaks
CREATE EXTENSION test_prolly;
CREATE SCHEMA ln VERSIONED;
CREATE TABLE ln.t (id int PRIMARY KEY, note text);
INSERT INTO ln.t VALUES (1, 'old secret');
SELECT length(dolt_commit('-Am', 'old table')::text) AS committed;
SELECT dolt_hashof('HEAD') AS old_commit \gset
-- (a branch that keeps the old table, with a row of its own)
SELECT dolt_branch('keeps_old');
SELECT dolt_checkout('keeps_old');
INSERT INTO ln.t VALUES (2, 'old secret, kept');
SELECT length(dolt_commit('-am', 'kept old')::text) AS committed;
SELECT dolt_checkout('main');
DROP TABLE ln.t;
SELECT length(dolt_commit('-Am', 'dropped')::text) AS committed;
CREATE TABLE ln.t (id int PRIMARY KEY, note text);
INSERT INTO ln.t VALUES (10, 'public');
CREATE ROLE regress_lb_reader;
CREATE ROLE regress_lb_writer IN ROLE pg_prolly_write;
GRANT USAGE ON SCHEMA ln TO regress_lb_reader, regress_lb_writer;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;
SELECT length(dolt_commit('-Am', 'new table')::text) AS committed;
SELECT dolt_hashof('HEAD') AS new_commit \gset

-- the reader reads the new table's history, not the old one's
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD', NULL::ln.t);
SELECT * FROM dolt_at(:'old_commit', NULL::ln.t);
-- (a head move is a write role's: the reader can't move one there itself)
SELECT dolt_reset('--soft', :'old_commit');
RESET ROLE;

-- a head moved to the old table's commit and then committed: the new
-- table recorded above the old one's commit, which isn't its version
SET ROLE regress_lb_writer;
SELECT dolt_reset('--soft', :'old_commit') IS NOT NULL AS moved;
SELECT length(dolt_commit('--allow-empty', '-m', 'over the old')::text) AS committed;
RESET ROLE;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD', NULL::ln.t);
SELECT * FROM dolt_at('HEAD~', NULL::ln.t);
SELECT * FROM dolt_at(:'old_commit', NULL::ln.t);
SELECT row FROM dolt_history_table(NULL::ln.t);
SELECT row FROM dolt_history_table('ln.t');
RESET ROLE;
-- (the database's owner reads it)
SELECT * FROM dolt_at('HEAD~', NULL::ln.t);
SELECT dolt_reset('--hard', :'new_commit') IS NOT NULL AS back;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;

-- a merge of a branch that still has the old table
SELECT (dolt_merge('--no-ff', '-m', 'merged keeps_old', 'keeps_old'))[3] AS conflicts;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD', NULL::ln.t) ORDER BY id;
SELECT * FROM dolt_at('HEAD^2', NULL::ln.t);
SELECT * FROM dolt_at('keeps_old', NULL::ln.t);
SELECT row FROM dolt_history_table(NULL::ln.t);
SELECT diff_type, to_row FROM dolt_diff('HEAD^2', 'HEAD', NULL::ln.t);
RESET ROLE;
SELECT dolt_reset('--hard', :'new_commit') IS NOT NULL AS back;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;

-- a merge of a branch made from the new table: its versions read
SELECT dolt_branch('keeps_new');
SELECT dolt_checkout('keeps_new');
INSERT INTO ln.t VALUES (11, 'public, side');
SELECT length(dolt_commit('-am', 'side row')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO ln.t VALUES (12, 'public, main');
SELECT length(dolt_commit('-am', 'main row')::text) AS committed;
SELECT (dolt_merge('-m', 'merged keeps_new', 'keeps_new'))[3] AS conflicts;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD^2', NULL::ln.t) ORDER BY id;
SELECT * FROM dolt_at('keeps_new', NULL::ln.t) ORDER BY id;
SELECT count(*) AS versions FROM dolt_history_table(NULL::ln.t);
RESET ROLE;

-- a head moved back along the table's own history and committed: still
-- its versions
SET ROLE regress_lb_writer;
SELECT dolt_reset('--soft', 'HEAD~') IS NOT NULL AS moved;
SELECT length(dolt_commit('--allow-empty', '-m', 'back a step')::text) AS committed;
RESET ROLE;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD~', NULL::ln.t) ORDER BY id;
RESET ROLE;

-- on a branch just made, before a commit of its own, the reader reads the
-- table's history; the head moved to the old table's commit and committed
-- is told apart there too
SELECT dolt_checkout('-b', 'fresh') IS NOT NULL AS made;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD~', NULL::ln.t) ORDER BY id;
RESET ROLE;
SET ROLE regress_lb_writer;
SELECT dolt_reset('--soft', :'old_commit') IS NOT NULL AS moved;
SELECT length(dolt_commit('--allow-empty', '-m', 'over the old, fresh')::text) AS committed;
RESET ROLE;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD~', NULL::ln.t);
RESET ROLE;
SELECT dolt_checkout('main') IS NOT NULL AS back;

-- a squash of the drop into the re-make, by a rebase: one commit whose
-- parent holds the old table, which wasn't made from this one
SELECT dolt_checkout('-b', 'squashed', :'old_commit') IS NOT NULL AS made;
DROP TABLE ln.t;
SELECT length(dolt_commit('-Am', 'dropped')::text) AS committed;
CREATE TABLE ln.t (id int PRIMARY KEY, note text);
INSERT INTO ln.t VALUES (20, 'public, squashed');
GRANT USAGE ON SCHEMA ln TO regress_lb_reader;
GRANT SELECT ON ln.t TO regress_lb_reader;
SELECT length(dolt_commit('-Am', 'made again')::text) AS committed;
SELECT dolt_rebase('-i', :'old_commit') IS NOT NULL AS began;
UPDATE dolt_rebase SET action = 'squash' WHERE commit_message = 'made again';
SELECT dolt_rebase('--continue') IS NOT NULL AS done;
SELECT active_branch();
SELECT dolt_hashof('HEAD~') = :'old_commit' AS on_old;
INSERT INTO ln.t VALUES (21, 'public, after');
SELECT length(dolt_commit('-am', 'after the squash')::text) AS committed;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD', NULL::ln.t) ORDER BY id;
SELECT * FROM dolt_at(:'old_commit', NULL::ln.t);
RESET ROLE;
SELECT dolt_checkout('main');

-- a branch made at the old table's branch's head (from its hash), merging
-- main in, and merged back: its first use copies keeps_old's relation, the
-- nearest source's, the old table's, so neither its commits nor the old
-- table's are this table's
SELECT dolt_hashof('main') AS before_k \gset
SELECT dolt_branch('k3', dolt_hashof('keeps_old')) IS NOT NULL AS made;
SELECT dolt_checkout('k3') IS NOT NULL AS on_k3;
SELECT (dolt_merge('-m', 'main into k3', 'main'))[3] AS conflicts;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('--no-ff', '-m', 'k3 in', 'k3'))[3] AS conflicts;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at(:'old_commit', NULL::ln.t);
SELECT * FROM dolt_at('keeps_old', NULL::ln.t);
SELECT * FROM dolt_at('HEAD^2^1', NULL::ln.t);
SELECT count(*) AS rows_now FROM ln.t;
-- (the merge brought in versions of the old table's relation, so it made
-- the table again, a relation of its own: main's history before it is
-- another relation's now, and is refused)
SELECT * FROM dolt_at('HEAD^1', NULL::ln.t) ORDER BY id;
SELECT * FROM dolt_at('k3', NULL::ln.t) ORDER BY id;
SELECT * FROM dolt_at('HEAD^2', NULL::ln.t) ORDER BY id;
RESET ROLE;
SELECT dolt_reset('--hard', :'before_k') IS NOT NULL AS back;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;

-- a branch made at the old table's commit after the re-make, first used
-- there, with a commit of its own, merged in (its copies are the nearest
-- source's relation's, here the old table's)
SELECT dolt_branch('late2', :'old_commit') IS NOT NULL AS made;
SELECT dolt_checkout('late2') IS NOT NULL AS on_late2;
INSERT INTO ln.t VALUES (3, 'late2 row');
SELECT length(dolt_commit('-am', 'late2 row')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('--no-ff', '-m', 'late2 in', 'late2'))[3] AS conflicts;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD^2', NULL::ln.t) ORDER BY id;
SELECT * FROM dolt_at('late2', NULL::ln.t) ORDER BY id;
SELECT * FROM dolt_at(:'old_commit', NULL::ln.t);
RESET ROLE;
SELECT count(*) AS owner_reads FROM dolt_at('late2', NULL::ln.t);
SELECT dolt_reset('--hard', :'before_k') IS NOT NULL AS back;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;

-- a branch made at the new table's commit by hash, its head moved to the
-- old table's commit by a write role and committed
SELECT dolt_branch('k4', :'new_commit') IS NOT NULL AS made;
SET ROLE regress_lb_writer;
SELECT dolt_checkout('k4') IS NOT NULL AS on_k4;
SELECT dolt_reset('--soft', :'old_commit') IS NOT NULL AS moved;
SELECT length(dolt_commit('--allow-empty', '-m', 'over the old, k4')::text) AS committed;
RESET ROLE;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD~', NULL::ln.t);
SELECT * FROM dolt_at('k4~', NULL::ln.t);
RESET ROLE;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;

-- a table's name can't forge a line of a commit's record: one whose name
-- holds a newline and the new table's line, made on the old table's
-- branch, leaves that branch's commits the old table's
SELECT dolt_hashof('main') AS before_f \gset
SELECT lpad('ln.t'::regclass::oid::text, 10, '0') AS new_relid \gset
SELECT dolt_checkout('keeps_old') IS NOT NULL AS on_old;
SELECT dolt_checkout('-b', 'forger') IS NOT NULL AS made;
SELECT format('CREATE TABLE ln.%I (id int PRIMARY KEY)',
			  E'a\n' || :'new_relid' || E' ln.t\nb') AS forge \gset
:forge;
SELECT length(dolt_commit('-Am', 'forged')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('--no-ff', '-m', 'forger in', 'forger'))[3] AS conflicts;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD^2', NULL::ln.t);
SELECT * FROM dolt_at('forger', NULL::ln.t);
RESET ROLE;
-- (the forging table's own history is its own, its name read back as it
-- was: merged in, it is of the class of the relation forger's record lists
-- under that name)
SELECT E'a\n' || :'new_relid' || E' ln.t\nb' AS forge_name \gset
SELECT format('GRANT SELECT ON ln.%I TO regress_lb_reader', :'forge_name') AS grant_forge \gset
:grant_forge;
SET ROLE regress_lb_reader;
SELECT format('SELECT count(*) AS forged_rows FROM dolt_at(%L, NULL::ln.%I)',
			  'HEAD^2', :'forge_name') AS read_forge \gset
:read_forge;
RESET ROLE;
SELECT dolt_reset('--hard', :'before_f') IS NOT NULL AS back;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;

-- a table made on a branch and merged in is a relation made here, of the
-- class of the branch's relation it was made from: its branch's history
-- is its grantee's
SELECT dolt_branch('feature') IS NOT NULL AS made;
SELECT dolt_checkout('feature') IS NOT NULL AS on_feature;
CREATE TABLE ln.f (id int PRIMARY KEY);
INSERT INTO ln.f VALUES (1);
SELECT length(dolt_commit('-Am', 'feature table')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
INSERT INTO ln.t VALUES (30, 'main, meanwhile');
SELECT length(dolt_commit('-am', 'meanwhile')::text) AS committed;
SELECT (dolt_merge('--no-ff', '-m', 'feature in', 'feature'))[3] AS conflicts;
GRANT SELECT ON ln.f TO regress_lb_reader;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD', NULL::ln.f);
SELECT * FROM dolt_at('HEAD^2', NULL::ln.f);
-- (the table here's own history reads, through the merge)
SELECT count(*) > 0 AS read FROM dolt_at('HEAD^1', NULL::ln.t);
RESET ROLE;
-- (and so for a table a branch's integration brings in: the merge it
-- makes into main is one of the branch's head)
SELECT dolt_branch('feature_i') IS NOT NULL AS made;
SELECT dolt_checkout('feature_i') IS NOT NULL AS on_feature_i;
CREATE TABLE ln.fi (id int PRIMARY KEY);
INSERT INTO ln.fi VALUES (1);
SELECT length(dolt_commit('-Am', 'integrated table')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
INSERT INTO ln.t VALUES (31, 'main, meanwhile again');
SELECT length(dolt_commit('-am', 'meanwhile again')::text) AS committed;
SELECT dolt_checkout('feature_i') IS NOT NULL AS on_feature_i;
SELECT dolt_integrate('-m', 'feature_i in') IS NOT NULL AS integrated;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
GRANT SELECT ON ln.fi TO regress_lb_reader;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD', NULL::ln.fi);
SELECT * FROM dolt_at('HEAD^2', NULL::ln.fi);
RESET ROLE;

-- a refused read reads each commit's record about once: a chain of the
-- old table's commits, merged with main's now and then
SELECT dolt_checkout('-b', 'chain', 'keeps_old') IS NOT NULL AS made;
DO $$
BEGIN
	FOR i IN 1..20 LOOP
		INSERT INTO ln.t VALUES (100 + i, 'old chain');
		PERFORM dolt_commit('-am', 'chain ' || i);
		IF i % 4 = 0 THEN
			PERFORM dolt_merge('-m', 'main into chain', 'main');
		END IF;
	END LOOP;
END $$;
SELECT count(*) AS chain_commits FROM dolt_log('chain') \gset
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT pg_stat_force_next_flush();
SELECT value AS read_before FROM pg_stat_prolly WHERE name = 'commits_read' \gset
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('chain', NULL::ln.t);
RESET ROLE;
SELECT pg_stat_force_next_flush();
SELECT value - :read_before <= 3 * :chain_commits AS linear
FROM pg_stat_prolly WHERE name = 'commits_read';

-- the old table's branch merging main in, then merged back (a
-- fast-forward): the merge is of the old table's relation, so the head is
-- no version of this one's
SELECT dolt_checkout('keeps_old') IS NOT NULL AS on_old;
SELECT (dolt_merge('-m', 'main into old', 'main'))[3] AS conflicts;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('keeps_old'))[2] AS fast_forward;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at(:'old_commit', NULL::ln.t);
SELECT * FROM dolt_at('HEAD^1', NULL::ln.t);
SELECT * FROM dolt_at('keeps_old~', NULL::ln.t);
SELECT diff_type, from_row FROM dolt_diff(:'old_commit', 'HEAD', NULL::ln.t);
-- (and the head, of the old table's relation, cuts the table's own
-- history off from the working set: it fails closed)
SELECT * FROM dolt_at(:'new_commit', NULL::ln.t);
RESET ROLE;

-- a merge, pick or revert made over a table dropped and made again in the
-- working set, uncommitted: the commit holds the old table (the working
-- set's own change left out), so it isn't recorded made from the new one
SELECT dolt_reset('--hard', :'before_k') IS NOT NULL AS back;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;
CREATE TABLE ln.sm (id int PRIMARY KEY, note text);
CREATE TABLE ln.sp (id int PRIMARY KEY, note text);
CREATE TABLE ln.sr (id int PRIMARY KEY, note text);
INSERT INTO ln.sm VALUES (1, 'old secret m');
INSERT INTO ln.sp VALUES (1, 'old secret p');
INSERT INTO ln.sr VALUES (1, 'old secret r');
SELECT length(dolt_commit('-Am', 'secrets')::text) AS committed;
SELECT dolt_branch('picks') IS NOT NULL AS made;
SELECT dolt_checkout('picks') IS NOT NULL AS on_picks;
INSERT INTO ln.t VALUES (40, 'picked');
SELECT length(dolt_commit('-am', 'to pick')::text) AS committed;
SELECT dolt_hashof('HEAD') AS to_pick \gset
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
INSERT INTO ln.t VALUES (41, 'to revert');
SELECT length(dolt_commit('-am', 'to revert')::text) AS committed;
SELECT dolt_hashof('HEAD') AS to_revert \gset
SELECT dolt_hashof('HEAD') AS secrets_head \gset
CREATE FUNCTION pg_temp.remake(t text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('DROP TABLE ln.%I', t);
	EXECUTE format('CREATE TABLE ln.%I (id int PRIMARY KEY, note text)', t);
	EXECUTE format('GRANT SELECT ON ln.%I TO regress_lb_reader', t);
END $$;
-- (a merge)
SELECT pg_temp.remake('sm');
SELECT (dolt_merge('--no-ff', '-m', 'picks in', 'picks'))[3] AS conflicts;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD', NULL::ln.sm);
RESET ROLE;
-- (nor through a snapshot mount of it)
CREATE SCHEMA lb_snap;
SELECT mount FROM prolly_mount('HEAD', 'ln', 'lb_snap', ARRAY['sm']);
GRANT USAGE ON SCHEMA lb_snap TO regress_lb_reader;
GRANT SELECT ON lb_snap.sm TO regress_lb_reader;
SET ROLE regress_lb_reader;
SELECT * FROM lb_snap.sm;
RESET ROLE;
SELECT prolly_unmount('lb_snap');
SELECT dolt_reset('--hard', :'secrets_head') IS NOT NULL AS back;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;
-- (a cherry-pick)
SELECT pg_temp.remake('sp');
SELECT (dolt_cherry_pick(:'to_pick'))[1] IS NOT NULL AS picked;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD', NULL::ln.sp);
RESET ROLE;
SELECT dolt_reset('--hard', :'secrets_head') IS NOT NULL AS back;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;
-- (a revert)
SELECT pg_temp.remake('sr');
SELECT (dolt_revert(:'to_revert'))[1] IS NOT NULL AS reverted;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD', NULL::ln.sr);
RESET ROLE;
SELECT dolt_reset('--hard', :'secrets_head') IS NOT NULL AS back;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;

-- the same, a definition changed by the move too (an unrelated table
-- altered): the uncommitted changes set aside, the branch moved to its
-- head, so the remade table holds the head's rows; still not recorded made
-- from it
CREATE TABLE ln.u (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'u')::text) AS committed;
SELECT dolt_branch('alters') IS NOT NULL AS made;
SELECT dolt_checkout('alters') IS NOT NULL AS on_alters;
ALTER TABLE ln.u ADD COLUMN c int;
SELECT length(dolt_commit('-am', 'alter to pick')::text) AS committed;
SELECT dolt_hashof('HEAD') AS alter_pick \gset
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
ALTER TABLE ln.u ADD COLUMN d int;
SELECT length(dolt_commit('-am', 'alter to revert')::text) AS committed;
SELECT dolt_hashof('HEAD') AS alter_revert \gset
SELECT dolt_hashof('HEAD') AS secrets2_head \gset
-- (a merge)
SELECT pg_temp.remake('sm');
SELECT (dolt_merge('--no-ff', '-m', 'alters in', 'alters'))[3] AS conflicts;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD', NULL::ln.sm);
RESET ROLE;
CREATE SCHEMA lb_snap2;
SELECT mount FROM prolly_mount('HEAD', 'ln', 'lb_snap2', ARRAY['sm']);
GRANT USAGE ON SCHEMA lb_snap2 TO regress_lb_reader;
GRANT SELECT ON lb_snap2.sm TO regress_lb_reader;
SET ROLE regress_lb_reader;
SELECT * FROM lb_snap2.sm;
RESET ROLE;
SELECT prolly_unmount('lb_snap2');
SELECT dolt_reset('--hard', :'secrets2_head') IS NOT NULL AS back;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;
-- (a cherry-pick)
SELECT pg_temp.remake('sp');
SELECT (dolt_cherry_pick(:'alter_pick'))[1] IS NOT NULL AS picked;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD', NULL::ln.sp);
RESET ROLE;
SELECT dolt_reset('--hard', :'secrets2_head') IS NOT NULL AS back;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;
-- (a revert)
SELECT pg_temp.remake('sr');
SELECT (dolt_revert(:'alter_revert'))[1] IS NOT NULL AS reverted;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD', NULL::ln.sr);
RESET ROLE;
SELECT dolt_reset('--hard', :'secrets2_head') IS NOT NULL AS back;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;

-- a branch first used after the table changed on main: its copies, moved
-- back to the table's definition at its start, are a relation made again
-- (a column renamed back), the same table's: its commits read
SELECT dolt_branch('late_side') IS NOT NULL AS made;
ALTER TABLE ln.t RENAME COLUMN note TO note2;
SELECT length(dolt_commit('-am', 'renamed note')::text) AS committed;
SELECT dolt_checkout('late_side') IS NOT NULL AS on_late;
INSERT INTO ln.t VALUES (60, 'late side');
SELECT length(dolt_commit('-am', 'late side row')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SET ROLE regress_lb_reader;
SELECT id FROM dolt_at('late_side', NULL::ln.t) WHERE id = 60;
RESET ROLE;
SELECT dolt_reset('--hard', :'secrets2_head') IS NOT NULL AS back;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;

-- a class is no branch's entry: a branch named as a relation's class is
-- keyed, its merge's conflicted names written and dropped, leaves it
CREATE TABLE ln.c (id int PRIMARY KEY, a int, note text);
GRANT SELECT ON ln.c TO regress_lb_reader;
SELECT length(dolt_commit('-Am', 'c')::text) AS committed;
SELECT dolt_branch('c_late') IS NOT NULL AS made;
ALTER TABLE ln.c DROP COLUMN a;
SELECT length(dolt_commit('-am', 'c without a')::text) AS committed;
SELECT dolt_checkout('c_late') IS NOT NULL AS on_late;
INSERT INTO ln.c VALUES (1, 1, 'late c');
SELECT length(dolt_commit('-am', 'late c row')::text) AS committed;
SELECT lpad('ln.c'::regclass::oid::text, 10, '0') AS c_relid \gset
SELECT test_refs_value('J' || :'c_relid') IS NOT NULL AS classed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SET ROLE regress_lb_reader;
SELECT id, note FROM dolt_at('c_late', NULL::ln.c);
RESET ROLE;
SELECT dolt_branch(:'c_relid') IS NOT NULL AS made;
SELECT dolt_branch('c_other') IS NOT NULL AS made;
SELECT dolt_checkout('c_other') IS NOT NULL AS on_other;
INSERT INTO ln.c VALUES (2, 'other');
SELECT length(dolt_commit('-am', 'other c')::text) AS committed;
SELECT dolt_checkout(:'c_relid') IS NOT NULL AS on_named;
INSERT INTO ln.c VALUES (2, 'mine');
SELECT length(dolt_commit('-am', 'my c')::text) AS committed;
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('c_other'))[3] AS conflicts;
SELECT dolt_merge('--abort') IS NOT NULL AS aborted;
RESET dolt.allow_commit_conflicts;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SET ROLE regress_lb_reader;
SELECT id, note FROM dolt_at('c_late', NULL::ln.c);
RESET ROLE;
SELECT dolt_reset('--hard', :'secrets2_head') IS NOT NULL AS back;
GRANT SELECT ON ln.t TO regress_lb_reader, regress_lb_writer;

-- a commit of dolt.transaction_commit's records its relations too: the
-- table's history reads through it
SET dolt.transaction_commit = on;
INSERT INTO ln.t VALUES (50, 'by transaction');
INSERT INTO ln.t VALUES (51, 'by transaction');
RESET dolt.transaction_commit;
SET ROLE regress_lb_reader;
SELECT count(*) AS rows_at_head FROM dolt_at('HEAD', NULL::ln.t) WHERE id >= 50;
SELECT count(*) AS rows_before FROM dolt_at('HEAD~', NULL::ln.t) WHERE id >= 50;
RESET ROLE;

-- an allowed read of another branch's history reads each commit's record
-- about once: a side branch's commits, each read
CREATE TABLE ln.h (id int PRIMARY KEY, note text);
GRANT SELECT ON ln.h TO regress_lb_reader;
SELECT length(dolt_commit('-Am', 'h')::text) AS committed;
SELECT dolt_branch('long_side') IS NOT NULL AS made;
SELECT dolt_checkout('long_side') IS NOT NULL AS on_side;
DO $$
BEGIN
	FOR i IN 1..30 LOOP
		INSERT INTO ln.h VALUES (200 + i, 'side');
		PERFORM dolt_commit('-am', 'side ' || i);
	END LOOP;
END $$;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT count(*) AS side_commits FROM dolt_log('long_side') \gset
CREATE SCHEMA lb_mnt;
SELECT mount FROM prolly_mount('long_side', 'ln', 'lb_mnt', ARRAY['h'], false, true);
GRANT USAGE ON SCHEMA lb_mnt TO regress_lb_reader;
GRANT SELECT ON lb_mnt.h TO regress_lb_reader;
SELECT pg_stat_force_next_flush();
SELECT value AS read_before FROM pg_stat_prolly WHERE name = 'commits_read' \gset
SET ROLE regress_lb_reader;
SELECT count(DISTINCT commit_hash) >= 30 AS side_versions_read FROM lb_mnt.h;
RESET ROLE;
SELECT pg_stat_force_next_flush();
SELECT value - :read_before AS side_read FROM pg_stat_prolly WHERE name = 'commits_read' \gset
SELECT :side_read <= 8 * :side_commits AS linear;
SELECT prolly_unmount('lb_mnt');

-- uncommitted changes set aside for a move that changes definitions, and
-- put back after it, are the branch's own: a table made again in the
-- working set doesn't join the class of the other side's table of its name
CREATE TABLE ln.p (id int PRIMARY KEY, note text);
CREATE TABLE ln.pu (id int PRIMARY KEY);
INSERT INTO ln.p VALUES (1, 'old secret p');
SELECT length(dolt_commit('-Am', 'old p')::text) AS committed;
SELECT dolt_hashof('HEAD') AS p_old \gset
SELECT dolt_branch('pside') IS NOT NULL AS made;
SELECT dolt_checkout('pside') IS NOT NULL AS on_pside;
ALTER TABLE ln.pu ADD COLUMN c int;
SELECT length(dolt_commit('-am', 'pside alter')::text) AS committed;
SELECT dolt_hashof('HEAD') AS p_alter \gset
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
ALTER TABLE ln.pu ADD COLUMN r int;
SELECT length(dolt_commit('-am', 'main alter')::text) AS committed;
SELECT dolt_hashof('HEAD') AS p_ralter \gset
DROP TABLE ln.p;
SELECT length(dolt_commit('-Am', 'dropped p')::text) AS committed;
SELECT dolt_hashof('HEAD') AS p_dropped \gset
-- (a cherry-pick: the table made again has no class of another's)
CREATE TABLE ln.p (id int PRIMARY KEY, note text);
SELECT (dolt_cherry_pick(:'p_alter'))[1] IS NOT NULL AS picked;
SELECT test_refs_value('J' || lpad('ln.p'::regclass::oid::text, 10, '0')) IS NULL
	AS own_class;
SELECT dolt_reset('--hard', :'p_dropped') IS NOT NULL AS back;
-- (a revert, of a commit whose parent has the old table: the same)
CREATE TABLE ln.p (id int PRIMARY KEY, note text);
SELECT (dolt_revert(:'p_ralter'))[1] IS NOT NULL AS reverted;
SELECT test_refs_value('J' || lpad('ln.p'::regclass::oid::text, 10, '0')) IS NULL
	AS own_class;
SELECT dolt_reset('--hard', :'p_dropped') IS NOT NULL AS back;
-- (a merge, then the side merged again: the side's commits, and the old
-- table's, stay another table's)
CREATE TABLE ln.p (id int PRIMARY KEY, note text);
INSERT INTO ln.p VALUES (10, 'public p');
SELECT (dolt_merge('--no-ff', '-m', 'pside in', 'pside'))[3] AS conflicts;
SELECT length(dolt_commit('-Am', 'new p')::text) AS committed;
SELECT dolt_checkout('pside') IS NOT NULL AS on_pside;
INSERT INTO ln.pu VALUES (1, 1);
SELECT length(dolt_commit('-am', 'pside row')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('--no-ff', '-m', 'pside in again', 'pside'))[3] AS conflicts;
GRANT SELECT ON ln.p TO regress_lb_reader;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD', NULL::ln.p);
SELECT * FROM dolt_at('HEAD^2', NULL::ln.p);
SELECT * FROM dolt_at('pside', NULL::ln.p);
SELECT * FROM dolt_at(:'p_old', NULL::ln.p);
RESET ROLE;

-- an uncommitted row change to the reader's table, set aside and put back
-- over such a merge, is the same table's: its history reads through it
SELECT dolt_checkout('pside') IS NOT NULL AS on_pside;
ALTER TABLE ln.pu ADD COLUMN e int;
SELECT length(dolt_commit('-am', 'pside alter again')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
INSERT INTO ln.p VALUES (11, 'public p, uncommitted');
SELECT (dolt_merge('--no-ff', '-m', 'pside in, over a row', 'pside'))[3] AS conflicts;
SET ROLE regress_lb_reader;
SELECT * FROM ln.p ORDER BY id;
SELECT * FROM dolt_at('HEAD', NULL::ln.p) ORDER BY id;
SELECT * FROM dolt_at('HEAD~', NULL::ln.p) ORDER BY id;
RESET ROLE;
SELECT length(dolt_commit('-am', 'the row')::text) AS committed;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD', NULL::ln.p) ORDER BY id;
SELECT * FROM dolt_at('HEAD~', NULL::ln.p) ORDER BY id;
RESET ROLE;

-- a collected commit's record goes after the collection; a live one's
-- stays
SELECT dolt_hashof('HEAD') AS live_commit \gset
SELECT dolt_branch('abandon') IS NOT NULL AS made;
SELECT dolt_checkout('abandon') IS NOT NULL AS on_abandon;
INSERT INTO ln.p VALUES (90, 'abandoned');
SELECT length(dolt_commit('-am', 'to abandon')::text) AS committed;
SELECT dolt_hashof('HEAD') AS abandoned \gset
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('-D', 'abandon') IS NOT NULL AS deleted;
SELECT test_refs_value('L' || :'abandoned') IS NOT NULL AS recorded;
SELECT dolt_gc() IS NOT NULL AS collected;
SELECT test_refs_walk_reaches(:'abandoned') AS reached,
	test_chunk_has(:'abandoned') AS stored,
	test_refs_value('L' || :'abandoned') IS NULL AS forgotten,
	test_refs_value('L' || :'live_commit') IS NOT NULL AS kept;
SET ROLE regress_lb_reader;
SELECT count(*) AS rows_live FROM dolt_at('HEAD', NULL::ln.p);
RESET ROLE;
-- (many at once are forgotten in one edit of the refs: what it appends
-- doesn't grow a tree path per record)
SELECT dolt_branch('abandon_many') IS NOT NULL AS made;
SELECT dolt_checkout('abandon_many') IS NOT NULL AS on_many;
DO $$
BEGIN
	FOR i IN 1..40 LOOP
		INSERT INTO ln.p VALUES (100 + i, 'abandoned');
		PERFORM dolt_commit('-am', 'to abandon ' || i);
	END LOOP;
END $$;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('-D', 'abandon_many') IS NOT NULL AS deleted;
SELECT pg_stat_force_next_flush();
SELECT value AS forgotten_before FROM pg_stat_prolly WHERE name = 'records_forgotten' \gset
SELECT value AS journal_before FROM pg_stat_prolly WHERE name = 'journal_bytes_written' \gset
SELECT dolt_gc() IS NOT NULL AS collected;
SELECT pg_stat_force_next_flush();
SELECT value - :forgotten_before AS forgotten
FROM pg_stat_prolly WHERE name = 'records_forgotten';
SELECT value - :journal_before < 20000 AS one_edit
FROM pg_stat_prolly WHERE name = 'journal_bytes_written';

-- commits made elsewhere (cloned) have no records: their versions are
-- read by full readers only, and stay so under a commit made here
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/lineage-' || txid_current() AS lb_url \gset
SELECT dolt_remote('add', 'origin', :'lb_url') IS NOT NULL AS added;
SELECT dolt_push('origin', 'main') IS NOT NULL AS pushed;
CREATE DATABASE regress_lineage_clone;
\c regress_lineage_clone
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT dolt_clone(:'lb_url') IS NOT NULL AS cloned;
GRANT USAGE ON SCHEMA ln TO regress_lb_reader;
GRANT SELECT ON ln.p TO regress_lb_reader;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD', NULL::ln.p);
SELECT * FROM dolt_at('HEAD~', NULL::ln.p);
RESET ROLE;
SELECT count(*) AS owner_reads FROM dolt_at('HEAD~', NULL::ln.p);
INSERT INTO ln.p VALUES (12, 'made here');
SELECT length(dolt_commit('-am', 'here')::text) AS committed;
SET ROLE regress_lb_reader;
SELECT count(*) AS rows_here FROM dolt_at('HEAD', NULL::ln.p);
SELECT * FROM dolt_at('HEAD~', NULL::ln.p);
RESET ROLE;
\c regress_lineage_breaks
SELECT dolt_remote('remove', 'origin') IS NOT NULL AS removed;

\c contrib_regression
DROP DATABASE regress_lineage_breaks;
DROP DATABASE regress_lineage_clone;
DROP ROLE regress_lb_reader, regress_lb_writer;
