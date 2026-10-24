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

-- a merge of a branch that still has the old table
SELECT (dolt_merge('--no-ff', '-m', 'merged keeps_old', 'keeps_old'))[3] AS conflicts;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at('HEAD', NULL::ln.t) ORDER BY id;
SELECT * FROM dolt_at('HEAD^2', NULL::ln.t);
SELECT * FROM dolt_at('keeps_old', NULL::ln.t);
SELECT row FROM dolt_history_table(NULL::ln.t);
SELECT diff_type, to_row FROM dolt_diff('HEAD^2', 'HEAD', NULL::ln.t);
RESET ROLE;
SELECT dolt_reset('--hard', :'new_commit') IS NOT NULL AS back;

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

-- a branch made at the old table's branch's head (from its hash: it has
-- copies of main's relation, moved to the old rows), merging main in, and
-- merged back: its commits are of this relation (and its rows, merged, are
-- the table's: #936), the old table's commits aren't
SELECT dolt_hashof('main') AS before_k \gset
SELECT dolt_branch('k3', dolt_hashof('keeps_old')) IS NOT NULL AS made;
SELECT dolt_checkout('k3') IS NOT NULL AS on_k3;
SELECT (dolt_merge('-m', 'main into k3', 'main'))[3] AS conflicts;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('--no-ff', '-m', 'k3 in', 'k3'))[3] AS conflicts;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at(:'old_commit', NULL::ln.t);
SELECT * FROM dolt_at('keeps_old', NULL::ln.t);
SELECT * FROM dolt_at('HEAD^2^1', NULL::ln.t);
SELECT count(*) AS rows_now FROM ln.t;
SELECT * FROM dolt_at('HEAD^1', NULL::ln.t) ORDER BY id;
RESET ROLE;
SELECT dolt_reset('--hard', :'before_k') IS NOT NULL AS back;

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

-- a table made on a branch and merged in is made again here, a relation
-- of its own: its branch's history isn't its grantee's (it fails closed)
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
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at(:'old_commit', NULL::ln.t);
SELECT * FROM dolt_at('HEAD^1', NULL::ln.t);
SELECT * FROM dolt_at('keeps_old~', NULL::ln.t);
SELECT diff_type, from_row FROM dolt_diff(:'old_commit', 'HEAD', NULL::ln.t);
-- (and the head, of the old table's relation, cuts the table's own
-- history off from the working set: it fails closed)
SELECT * FROM dolt_at(:'new_commit', NULL::ln.t);
RESET ROLE;

\c contrib_regression
DROP DATABASE regress_lineage_breaks;
DROP ROLE regress_lb_reader, regress_lb_writer;
