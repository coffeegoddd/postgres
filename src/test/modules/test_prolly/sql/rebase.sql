-- dolt_rebase: a branch's commits replayed onto another commit, as
-- Dolt's: at once, or interactively by a plan in the dolt_rebase view
-- (pick, reword, squash, fixup, drop, edit), stopping at conflicts and
-- edits until --continue; --abort puts the branch back.

-- (a database of its own: its history is the test's alone)
CREATE DATABASE regress_rebase;
\c regress_rebase
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SET dolt.author = 'Rebaser <r@example.com>';

-- refused: nothing in progress, nothing to rebase, bad arguments
SELECT dolt_rebase('--abort');
SELECT dolt_rebase('--continue');
SELECT dolt_rebase('main');
SELECT dolt_rebase('-i');
SELECT dolt_rebase('-i', 'main1', 'main2');
SELECT dolt_rebase('--abrot');
SELECT dolt_rebase('-i', 'doesnotexist');
SELECT dolt_rebase('--empty=sometimes', 'main');
SELECT * FROM dolt_rebase;

CREATE SCHEMA rb VERSIONED;
CREATE TABLE rb.t (pk int PRIMARY KEY, v text);
SELECT dolt_commit('-Am', 'creating table t') IS NOT NULL AS committed;
SELECT dolt_branch('feature');
INSERT INTO rb.t VALUES (0, 'main');
SELECT dolt_commit('-am', 'main row 0') IS NOT NULL AS committed;

-- a non-interactive rebase: the branch's commits on main's head
SELECT dolt_checkout('feature') IS NOT NULL AS on_feature;
INSERT INTO rb.t VALUES (1, 'f');
SELECT dolt_commit('-am', 'feature row 1') IS NOT NULL AS committed;
INSERT INTO rb.t VALUES (2, 'f');
SELECT dolt_commit('-am', 'feature row 2') IS NOT NULL AS committed;
SELECT dolt_hashof('HEAD') AS before \gset
SELECT dolt_rebase('main');
SELECT active_branch();
SELECT pk FROM rb.t ORDER BY pk;
SELECT message, committer FROM dolt_log ORDER BY commit_order DESC;
SELECT dolt_hashof('feature~2') = dolt_hashof('main') AS on_main;
SELECT name FROM dolt_branches ORDER BY name;

-- refused over uncommitted changes
INSERT INTO rb.t VALUES (3, 'dirty');
SELECT dolt_rebase('main');
DELETE FROM rb.t WHERE pk = 3;

-- an interactive rebase, every action
SELECT dolt_checkout('-b', 'b1', 'main~1') IS NOT NULL AS on_b1;
INSERT INTO rb.t VALUES (10, 'x');
SELECT dolt_commit('-am', 'row 10') IS NOT NULL AS committed;
INSERT INTO rb.t VALUES (11, 'x');
SELECT dolt_commit('-am', 'row 11') IS NOT NULL AS committed;
INSERT INTO rb.t VALUES (12, 'x');
SELECT dolt_commit('-am', 'row 12') IS NOT NULL AS committed;
INSERT INTO rb.t VALUES (13, 'x');
SELECT dolt_commit('-am', 'row 13') IS NOT NULL AS committed;
INSERT INTO rb.t VALUES (14, 'x');
SELECT dolt_commit('-am', 'row 14') IS NOT NULL AS committed;
INSERT INTO rb.t VALUES (15, 'x');
SELECT dolt_commit('-am', 'row 15') IS NOT NULL AS committed;
SELECT dolt_rebase('-i', 'main');
SELECT active_branch();
SELECT rebase_order, action, commit_message FROM dolt_rebase ORDER BY rebase_order;
-- (the plan names the branch's commits)
SELECT bool_and(r.commit_hash = l.commit_hash) AS commits
	FROM dolt_rebase r JOIN dolt_log('b1') l ON l.message = r.commit_message;
UPDATE dolt_rebase SET rebase_order = 6.1 WHERE rebase_order = 6;
UPDATE dolt_rebase SET action = 'squash' WHERE rebase_order IN (2, 3);
UPDATE dolt_rebase SET action = 'drop' WHERE rebase_order = 4;
UPDATE dolt_rebase SET action = 'reword', commit_message = 'reworded!' WHERE rebase_order = 5;
UPDATE dolt_rebase SET action = 'fixup' WHERE rebase_order = 6.10;
UPDATE dolt_rebase SET action = 'shuffle' WHERE rebase_order = 1;
UPDATE dolt_rebase SET rebase_order = 2 WHERE rebase_order = 1;
SELECT rebase_order, action, commit_message FROM dolt_rebase ORDER BY rebase_order;
SELECT dolt_rebase('--continue');
SELECT dolt_rebase('--continue');
SELECT * FROM dolt_rebase;
SELECT active_branch();
SELECT name FROM dolt_branches ORDER BY name;
SELECT message FROM dolt_log ORDER BY commit_order DESC;
SELECT pk FROM rb.t ORDER BY pk;

-- an invalid plan: a squash with nothing picked before it
SELECT dolt_checkout('-b', 'b2', 'main~1') IS NOT NULL AS on_b2;
INSERT INTO rb.t VALUES (20, 'x');
SELECT dolt_commit('-am', 'row 20') IS NOT NULL AS committed;
INSERT INTO rb.t VALUES (21, 'x');
SELECT dolt_commit('-am', 'row 21') IS NOT NULL AS committed;
SELECT dolt_rebase('-i', 'main') IS NOT NULL AS started;
UPDATE dolt_rebase SET action = 'squash' WHERE rebase_order = 1;
SELECT dolt_rebase('--continue');
-- rows added to the plan and taken out of it
UPDATE dolt_rebase SET action = 'pick' WHERE rebase_order = 1;
DELETE FROM dolt_rebase WHERE rebase_order = 2;
INSERT INTO dolt_rebase VALUES (0.5, 'pick', 'abc', 'x');
SELECT rebase_order, action, commit_message FROM dolt_rebase ORDER BY rebase_order;
-- aborted: back on the branch as it was, the rebase's branch gone
SELECT dolt_rebase('--abort');
SELECT active_branch();
SELECT pk FROM rb.t ORDER BY pk;
SELECT name FROM dolt_branches ORDER BY name;
SELECT * FROM dolt_rebase;

-- a commit that becomes empty: dropped by default, kept with --empty=keep
SELECT dolt_checkout('-b', 'b3', 'main~1') IS NOT NULL AS on_b3;
INSERT INTO rb.t VALUES (0, 'main');
SELECT dolt_commit('-am', 'same as main') IS NOT NULL AS committed;
INSERT INTO rb.t VALUES (30, 'x');
SELECT dolt_commit('-am', 'row 30') IS NOT NULL AS committed;
SELECT dolt_branch('b3keep');
SELECT dolt_rebase('main');
SELECT message FROM dolt_log ORDER BY commit_order DESC LIMIT 3;
SELECT dolt_checkout('b3keep') IS NOT NULL AS on_b3keep;
SELECT dolt_rebase('--empty=keep', 'main');
SELECT message FROM dolt_log ORDER BY commit_order DESC LIMIT 3;

-- an edit stops after its pick; changes made then are a commit of their own
SELECT dolt_checkout('-b', 'b4', 'main~1') IS NOT NULL AS on_b4;
INSERT INTO rb.t VALUES (40, 'x');
SELECT dolt_commit('-am', 'row 40') IS NOT NULL AS committed;
INSERT INTO rb.t VALUES (41, 'x');
SELECT dolt_commit('-am', 'row 41') IS NOT NULL AS committed;
SELECT dolt_rebase('-i', 'main') IS NOT NULL AS started;
UPDATE dolt_rebase SET action = 'edit' WHERE rebase_order = 1;
SELECT (dolt_rebase('--continue'))[2] LIKE 'edit action paused at commit %' AS paused;
SELECT pk FROM rb.t ORDER BY pk;
INSERT INTO rb.t VALUES (42, 'edited');
SELECT dolt_rebase('--continue');
SELECT message FROM dolt_log ORDER BY commit_order DESC LIMIT 4;
SELECT pk FROM rb.t ORDER BY pk;

-- a conflict stops the rebase, in a transaction block; resolved, it goes on
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
UPDATE rb.t SET v = 'main' WHERE pk = 0;
INSERT INTO rb.t VALUES (50, 'main');
SELECT dolt_commit('-am', 'main row 50') IS NOT NULL AS committed;
SELECT dolt_checkout('-b', 'b5', 'main~1') IS NOT NULL AS on_b5;
INSERT INTO rb.t VALUES (50, 'b5');
SELECT dolt_commit('-am', 'b5 row 50') IS NOT NULL AS committed;
INSERT INTO rb.t VALUES (51, 'b5');
SELECT dolt_commit('-am', 'b5 row 51') IS NOT NULL AS committed;
-- (outside a block a conflict rolls the whole rebase back, as Dolt aborts it)
SELECT dolt_rebase('main');
SELECT active_branch();
BEGIN;
SELECT r[1], r[2] LIKE 'data conflict detected while rebasing commit %' AS conflict
	FROM dolt_rebase('main') r;
SELECT active_branch();
SELECT dolt_rebase('--continue');
ROLLBACK;
-- (stopped: another rebase, or a merge, refused; abort goes back)
BEGIN;
SELECT (dolt_rebase('main'))[1] AS stopped;
SELECT dolt_rebase('main');
ROLLBACK;
BEGIN;
SELECT (dolt_rebase('main'))[1] AS stopped;
SELECT dolt_rebase('--abort');
SELECT active_branch(), (SELECT count(*) FROM dolt_branches WHERE name = 'dolt_rebase_b5') AS left;
COMMIT;
BEGIN;
SELECT (dolt_rebase('main'))[1] AS stopped;
SELECT dolt_conflicts_resolve('--theirs', 'rb.t');
SELECT dolt_rebase('--continue');
COMMIT;
SELECT active_branch();
SELECT pk, v FROM rb.t WHERE pk >= 50 ORDER BY pk;
SELECT message, committer FROM dolt_log ORDER BY commit_order DESC LIMIT 3;

-- the rebased branch moved meanwhile: refused at the end
SELECT dolt_checkout('-b', 'b6', 'main~1') IS NOT NULL AS on_b6;
INSERT INTO rb.t VALUES (60, 'x');
SELECT dolt_commit('-am', 'row 60') IS NOT NULL AS committed;
SELECT dolt_rebase('-i', 'main') IS NOT NULL AS started;
SELECT dolt_branch('-f', 'b6', 'main');
SELECT dolt_rebase('--continue');
SELECT dolt_rebase('--abort');

-- the working branch named already: refused
SELECT dolt_checkout('-b', 'b7', 'main~1') IS NOT NULL AS on_b7;
INSERT INTO rb.t VALUES (70, 'x');
SELECT dolt_commit('-am', 'row 70') IS NOT NULL AS committed;
SELECT dolt_branch('dolt_rebase_b7');
SELECT dolt_rebase('main');

-- a rebase in progress outlives its session, and is the branch's
\c regress_rebase
SELECT dolt_checkout('b7') IS NOT NULL AS on_b7;
SELECT dolt_branch('-D', 'dolt_rebase_b7');
SELECT dolt_rebase('-i', 'main') IS NOT NULL AS started;
\c regress_rebase
SELECT dolt_checkout('dolt_rebase_b7') IS NOT NULL AS back;
SELECT action, commit_message FROM dolt_rebase;
SELECT dolt_rebase('--continue');

-- another session's uncommitted changes on the rebased branch: refused at
-- the end, as Dolt's, not dropped by the branch's move
SELECT dolt_checkout('-b', 'b8', 'main~1') IS NOT NULL AS on_b8;
INSERT INTO rb.t VALUES (80, 'x');
SELECT dolt_commit('-am', 'row 80') IS NOT NULL AS committed;
SELECT dolt_rebase('-i', 'main') IS NOT NULL AS started;
\c regress_rebase
SELECT dolt_checkout('b8') IS NOT NULL AS on_b8;
INSERT INTO rb.t VALUES (81, 'another session');
\c regress_rebase
SELECT dolt_checkout('dolt_rebase_b8') IS NOT NULL AS back;
SELECT dolt_rebase('--continue');
SELECT dolt_rebase('--abort');
SELECT pk, v FROM rb.t WHERE pk >= 80 ORDER BY pk;

-- a plan step naming a commit that doesn't exist: refused as it is given
SELECT dolt_reset('--hard', 'HEAD') IS NOT NULL AS clean;
SELECT dolt_rebase('-i', 'main') IS NOT NULL AS started;
INSERT INTO dolt_rebase VALUES (5, 'pick', repeat('de', 20), 'x');
UPDATE dolt_rebase SET commit_hash = repeat('be', 20) WHERE rebase_order = 1;

-- the step an edit stopped at taken out of the plan: refused, not lost
UPDATE dolt_rebase SET action = 'edit' WHERE rebase_order = 1;
SELECT (dolt_rebase('--continue'))[2] LIKE 'edit action paused at commit %' AS paused;
INSERT INTO rb.t VALUES (82, 'edited');
DELETE FROM dolt_rebase WHERE rebase_order = 1;
SELECT dolt_rebase('--continue');
SELECT dolt_rebase('--abort');

-- dolt_commit --amend: the head replaced by a commit of the working set
-- with its parents, its message kept unless one is given
SELECT active_branch();
SELECT dolt_reset('--hard', 'HEAD') IS NOT NULL AS clean;
SELECT dolt_checkout('-b', 'amending') IS NOT NULL AS on_amending;
INSERT INTO rb.t VALUES (90, 'first');
SELECT dolt_commit('-am', 'row 90') IS NOT NULL AS committed;
SELECT dolt_hashof('HEAD~1') AS parent \gset
UPDATE rb.t SET v = 'amended' WHERE pk = 90;
SELECT dolt_commit('--amend') IS NOT NULL AS amended;
SELECT message FROM dolt_log LIMIT 1;
SELECT dolt_hashof('HEAD~1') = :'parent' AS same_parent;
SELECT count(*) AS changes FROM dolt_status;
SELECT v FROM dolt_at('HEAD', NULL::rb.t) WHERE pk = 90;
SELECT dolt_commit('--amend', '-m', 'row 90, reworded') IS NOT NULL AS amended;
SELECT message FROM dolt_log LIMIT 2;
SELECT dolt_hashof('HEAD~1') = :'parent' AS same_parent;
-- refused with a merge in progress
SELECT dolt_checkout('-b', 'amend_other', 'HEAD~1') IS NOT NULL AS on_other;
INSERT INTO rb.t VALUES (90, 'other');
SELECT dolt_commit('-am', 'other row 90') IS NOT NULL AS committed;
BEGIN;
SELECT m[3] AS conflicts FROM dolt_merge('amending') m;
SELECT dolt_commit('--amend', '-m', 'mid-merge');
ROLLBACK;

-- commits that change the schema, rebased: each pick merges its
-- definitions as a cherry-pick does
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
CREATE TABLE rb.s (pk int PRIMARY KEY, a text);
INSERT INTO rb.s VALUES (1, 'one'), (2, 'two');
SELECT dolt_commit('-Am', 'creating table s') IS NOT NULL AS committed;
SELECT dolt_branch('schemas');
INSERT INTO rb.s VALUES (3, 'three');
SELECT dolt_commit('-am', 'main row 3 of s') IS NOT NULL AS committed;
SELECT dolt_checkout('schemas') IS NOT NULL AS on_schemas;
ALTER TABLE rb.s ADD COLUMN b int NOT NULL DEFAULT 0;
SELECT dolt_commit('-am', 'adding b') IS NOT NULL AS committed;
UPDATE rb.s SET b = pk * 10;
SELECT dolt_commit('-am', 'filling b') IS NOT NULL AS committed;
ALTER TABLE rb.s RENAME COLUMN a TO name;
SELECT dolt_commit('-am', 'renaming a') IS NOT NULL AS committed;
CREATE INDEX s_b ON rb.s (b);
SELECT dolt_commit('-am', 'indexing b') IS NOT NULL AS committed;
SELECT dolt_rebase('main');
SELECT pk, name, b FROM rb.s ORDER BY pk;
SELECT indexname FROM pg_indexes WHERE schemaname = 'rb' AND tablename = 's' ORDER BY 1;
SELECT message FROM dolt_log LIMIT 5;
-- reordered so a pick's definitions conflict with the head's: an error,
-- which rolls the --continue back, the rebase left at its plan to change
-- or abort (Dolt aborts it)
SELECT dolt_branch('schemas2', 'HEAD~4');
SELECT dolt_checkout('schemas2') IS NOT NULL AS on_schemas2;
INSERT INTO rb.s VALUES (4, 'four');
SELECT dolt_commit('-am', 'row 4 of s') IS NOT NULL AS committed;
ALTER TABLE rb.s ADD COLUMN c text NOT NULL DEFAULT '';
SELECT dolt_commit('-am', 'adding c') IS NOT NULL AS committed;
ALTER TABLE rb.s ALTER COLUMN c DROP NOT NULL;
SELECT dolt_commit('-am', 'altering c') IS NOT NULL AS committed;
SELECT dolt_rebase('-i', 'main') IS NOT NULL AS started;
UPDATE dolt_rebase SET rebase_order = 3.1 WHERE rebase_order = 2;
\set VERBOSITY terse
SELECT dolt_rebase('--continue');
\set VERBOSITY default
SELECT active_branch();
SELECT rebase_order, action, commit_message FROM dolt_rebase ORDER BY rebase_order;
SELECT dolt_rebase('--abort');
SELECT active_branch();
SELECT message FROM dolt_log LIMIT 1;

\c postgres
DROP DATABASE regress_rebase;
