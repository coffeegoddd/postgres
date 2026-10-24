-- A history read takes a version of a table for the table here only over
-- its lineage: the name never without a table between.  Commits name tables
-- by name and identity alone, so a table dropped and made again with its
-- name looks continuous to them wherever a commit records the new table
-- above one holding the old: a head moved to the old table's commit and
-- then committed, a merge of a branch that still has it, a squash of the
-- drop into the re-make.  The branch's record of its tables tells them
-- apart: a commit whose parent isn't the record's commit vouches for the
-- parent's version only if both share a history the name stays in (#862).
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

-- a branch just made has its source's record of its tables: on it, the
-- head moved to the old table's commit and committed is told apart too,
-- and before a commit of its own the reader reads the table's history
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
-- parent holds the old table; the commit after it doesn't vouch for that
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

-- the old table's branch merging main in, then merged back (a
-- fast-forward): its merge's note is of the old table's relation, not
-- this one's, so it vouches for no parent of its
SELECT dolt_checkout('keeps_old') IS NOT NULL AS on_old;
SELECT (dolt_merge('-m', 'main into old', 'main'))[3] AS conflicts;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_merge('keeps_old'))[2] AS fast_forward;
SET ROLE regress_lb_reader;
SELECT * FROM dolt_at(:'old_commit', NULL::ln.t);
SELECT * FROM dolt_at('HEAD^1', NULL::ln.t);
SELECT * FROM dolt_at('keeps_old~', NULL::ln.t);
SELECT diff_type, from_row FROM dolt_diff(:'old_commit', 'HEAD', NULL::ln.t);
RESET ROLE;

\c contrib_regression
DROP DATABASE regress_lineage_breaks;
DROP ROLE regress_lb_reader, regress_lb_writer;
