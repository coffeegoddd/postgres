-- Writer modes: in the shared mode (the default) every session writes a
-- branch; in the owned mode a dirty branch is one writer's (dolt.writer),
-- claimed by its first write and given up when it is clean again.
CREATE SCHEMA wm VERSIONED;
CREATE TABLE wm.t (id int PRIMARY KEY, v text);
INSERT INTO wm.t VALUES (1, 'base');
CREATE ROLE wm_other;
CREATE ROLE wm_carol;
GRANT USAGE ON SCHEMA wm TO wm_other, wm_carol;
GRANT ALL ON wm.t TO wm_other, wm_carol;
CREATE SEQUENCE wm.s2;
SELECT length(dolt_commit('-m', 'wm base')::text) AS committed;
SELECT dolt_writer_mode();

-- Shared: writers mix, and nobody owns the branch.
SET dolt.writer = 'alice';
INSERT INTO wm.t VALUES (2, 'alice');
SET dolt.writer = 'bob';
INSERT INTO wm.t VALUES (3, 'bob');
SELECT name, dirty, owner, owner_role FROM dolt_branches() WHERE name = 'main';
SELECT dolt_reset('--hard');

-- Owned: the first write claims the branch, as a catalog row of the
-- transaction (gone if it aborts).
SELECT dolt_writer_mode('owned');
SELECT dolt_writer_mode();
SET dolt.writer = 'alice';
BEGIN;
INSERT INTO wm.t VALUES (2, 'alice');
ROLLBACK;
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';
INSERT INTO wm.t VALUES (2, 'alice');
SELECT name, dirty, owner, owner_role = current_user AS owner_role
FROM dolt_branches() WHERE name = 'main';

-- The owner writes on; anyone else is refused at once, rows and
-- definitions and sequences alike.
UPDATE wm.t SET v = 'alice again' WHERE id = 2;
SET dolt.writer = 'bob';
INSERT INTO wm.t VALUES (3, 'bob');
CREATE TABLE wm.u (id int);
CREATE SEQUENCE wm.s;
SELECT dolt_commit('-m', 'bob commits alice''s work');
SELECT * FROM wm.t ORDER BY id;

-- Reads and unversioned writes are anyone's.
CREATE TABLE wm_plain (id int);
INSERT INTO wm_plain VALUES (1);
DROP TABLE wm_plain;

-- A transaction is one writer's: it can't change writer once it has
-- written.
SET dolt.writer = 'alice';
BEGIN;
INSERT INTO wm.t VALUES (4, 'alice');
SET LOCAL dolt.writer = 'bob';
ROLLBACK;
SET dolt.writer = 'not a writer!';
SET dolt.writer = 'a_writer_name_far_longer_than_thirty_two_bytes';

-- The owner's commit makes the branch clean, and anyone's to claim.
SELECT length(dolt_commit('-m', 'alice''s')::text) AS committed;
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';
SET dolt.writer = 'bob';
INSERT INTO wm.t VALUES (3, 'bob');
SELECT name, owner FROM dolt_branches() WHERE name = 'main';

-- A reset by the owner discards the changes and the claim.
SELECT dolt_reset('--hard');
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';

-- A commit in the middle of a transaction gives the branch up, and the
-- transaction's next write claims it again.
BEGIN;
INSERT INTO wm.t VALUES (30, 'bob');
SELECT length(dolt_commit('-m', 'bob, mid-transaction')::text) AS committed;
SELECT name, owner FROM dolt_branches() WHERE name = 'main';
INSERT INTO wm.t VALUES (31, 'bob');
COMMIT;
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';
SELECT dolt_reset('--hard');
-- (taking a clean branch over takes nothing: the next write claims it)
SELECT dolt_branch('--take', 'main');
SELECT name, owner FROM dolt_branches() WHERE name = 'main';

-- An aborted merge is a reset: the owner's, and it gives the branch up.
SELECT dolt_branch('wm_c');
SELECT dolt_checkout('wm_c');
UPDATE wm.t SET v = 'on c' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'wm c')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE wm.t SET v = 'on main' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'wm main')::text) AS committed;
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('wm_c'))[3] AS conflicts;
RESET dolt.allow_commit_conflicts;
SELECT name, owner FROM dolt_branches() WHERE name = 'main';
SET dolt.writer = 'alice';
SELECT dolt_merge('--abort');
SET dolt.writer = 'bob';
SELECT dolt_merge('--abort') IS NOT NULL AS aborted;
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';
SELECT dolt_branch('-D', 'wm_c');

-- A write that changes nothing still claims (the branch counts as dirty
-- until a commit or reset says otherwise).
INSERT INTO wm.t VALUES (5, 'gone');
DELETE FROM wm.t WHERE id = 5;
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';
SET dolt.writer = 'alice';
INSERT INTO wm.t VALUES (6, 'alice');
SELECT dolt_reset('--hard');	-- (the database's owner discards bob's)
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';

-- Another role can't write as a writer that is a role's, nor take a
-- branch over without owning the database or being the role.
SET ROLE wm_carol;
SET dolt.writer = 'carol';
INSERT INTO wm.t VALUES (7, 'carol');
SET ROLE wm_other;
INSERT INTO wm.t VALUES (8, 'carol, spoofed');
SELECT dolt_branch('--take', 'main');
SET dolt.writer = 'dave';
INSERT INTO wm.t VALUES (8, 'dave');
RESET ROLE;

-- The database's owner takes it over: carol's changes become dave's.
SET dolt.writer = 'dave';
SELECT dolt_branch('--take', 'main');
SELECT name, owner FROM dolt_branches() WHERE name = 'main';
INSERT INTO wm.t VALUES (8, 'dave');
SELECT length(dolt_commit('-m', 'carol''s and dave''s')::text) AS committed;
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';

-- Branches are owned each on their own.
SELECT dolt_checkout('-b', 'wm_f');
INSERT INTO wm.t VALUES (9, 'dave on f');
SET dolt.writer = 'erin';
SELECT dolt_checkout('main');
INSERT INTO wm.t VALUES (9, 'erin on main');
SELECT name, owner FROM dolt_branches() WHERE name IN ('main', 'wm_f') ORDER BY name;
SELECT dolt_checkout('wm_f');
INSERT INTO wm.t VALUES (10, 'erin on f');
SET dolt.writer = 'dave';
SELECT dolt_reset('--hard');
SELECT dolt_checkout('main');
SET dolt.writer = 'erin';
SELECT dolt_reset('--hard');
SELECT dolt_branch('-D', 'wm_f');

-- Branch-only: main is protected, written only by integrations, which
-- commit as they change it.
SELECT dolt_writer_mode('branch-only');
SET dolt.writer = 'frank';
SELECT name, protected FROM dolt_branches() WHERE name = 'main';
INSERT INTO wm.t VALUES (11, 'frank on main');
CREATE TABLE wm.v (id int);
SELECT dolt_commit('-m', 'nothing on main');
SELECT dolt_checkout('-b', 'wm_frank');
SELECT name, protected FROM dolt_branches() WHERE name IN ('main', 'wm_frank') ORDER BY name;
INSERT INTO wm.t VALUES (11, 'frank');
SELECT (dolt_integrate('-m', 'frank''s first'))[2] AS fast_forward;
SELECT dolt_hashof('main') = dolt_hashof('wm_frank') AS same_head,
	(SELECT message FROM dolt_log('main') LIMIT 1) AS message;
SELECT name, dirty, owner FROM dolt_branches() WHERE name IN ('main', 'wm_frank') ORDER BY name;
SELECT id, v FROM wm.t WHERE id = 11;

-- Another writer's branch, from before; its change conflicts with
-- frank's once frank's is in main.
SELECT dolt_branch('wm_gina', 'main~1');
SET dolt.writer = 'gina';
SELECT dolt_checkout('wm_gina');
INSERT INTO wm.t VALUES (11, 'gina');
SELECT dolt_integrate('main', '-m', 'gina''s');
-- (all of it undone: gina's commit too, and gina is on her branch)
SELECT active_branch();
SELECT name, dirty, owner FROM dolt_branches() WHERE name IN ('main', 'wm_gina') ORDER BY name;
SELECT (SELECT count(*) FROM dolt_log('wm_gina')) = (SELECT count(*) FROM dolt_log('main')) - 1 AS not_committed;
-- (a merge into main that would stop with conflicts is refused, too)
SELECT length(dolt_commit('-m', 'gina''s')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT dolt_merge('wm_gina');
ROLLBACK;
-- resolved on her branch, it integrates
SELECT dolt_checkout('wm_gina');
BEGIN;
SELECT (dolt_merge('main'))[3] AS conflicts;
SELECT dolt_conflicts_resolve('--ours', 'wm.t');
SELECT length(dolt_commit('-m', 'gina merges main')::text) AS committed;
COMMIT;
SELECT (dolt_integrate('-m', 'gina''s, resolved'))[2] AS fast_forward;
SELECT id, v FROM wm.t WHERE id = 11;
SELECT dolt_checkout('main');
SELECT id, v FROM wm.t WHERE id = 11;

-- A reset of main is an integration too; dolt_integrate isn't run on
-- the branch it integrates into.
SELECT dolt_integrate('-m', 'on main');
SELECT dolt_reset('--hard', 'HEAD');

-- An integration doesn't open the branch to the transaction's other writes;
-- moving a protected branch to another commit is the database owner's.
BEGIN;
SELECT dolt_reset('--hard');
INSERT INTO wm.t VALUES (13, 'after an integration');
ROLLBACK;
SET ROLE wm_other;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_reset('--hard', 'HEAD') IS NOT NULL AS reset_to_head;
RESET ROLE;
-- Once an integration is done, a checkout after it isn't undone if the
-- transaction aborts.
SELECT dolt_checkout('-b', 'wm_ivy');
INSERT INTO wm.t VALUES (14, 'ivy');
BEGIN;
SELECT (dolt_integrate('-m', 'ivy''s'))[2] AS fast_forward;
SELECT dolt_checkout('main');
SELECT 1 / 0;
ROLLBACK;
SELECT active_branch();

-- Protection by branch, in any mode: main unprotected, and others
-- protected; only the database's owner sets it, and deletes a protected
-- branch; a copy is protected as its source.
SELECT dolt_branch('--unprotect', 'main');
INSERT INTO wm.t VALUES (12, 'gina on main');
SELECT dolt_reset('--hard');
SELECT dolt_branch('--protect', 'main');
SELECT dolt_branch('--protect', 'wm_frank');
SELECT dolt_branch('-c', 'wm_frank', 'wm_frank2');
SELECT name, protected FROM dolt_branches() WHERE name LIKE 'wm_%' ORDER BY name;
SET ROLE wm_other;
SELECT dolt_branch('--unprotect', 'wm_frank');
SELECT dolt_branch('-D', 'wm_frank');
RESET ROLE;
SELECT dolt_branch('--protect', 'no_such_branch');
-- (a branch with changes isn't protected: they'd be any integration's)
SELECT dolt_checkout('wm_ivy');
INSERT INTO wm.t VALUES (15, 'ivy, uncommitted');
SELECT dolt_checkout('main');
SELECT dolt_branch('--protect', 'wm_ivy');
SELECT dolt_checkout('wm_ivy');
SELECT dolt_reset('--hard');
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'wm_ivy');
SELECT dolt_branch('-D', 'wm_frank', 'wm_frank2', 'wm_gina');
SELECT dolt_branch('wm_frank');
SELECT name, protected FROM dolt_branches() WHERE name = 'wm_frank';
SELECT dolt_branch('-D', 'wm_frank');
-- (as the mode has it, a branch's protection is the mode's again)
SELECT dolt_branch('--unprotect', 'main');
SELECT dolt_branch('--protect', 'main');
SELECT dolt_writer_mode('owned');
SELECT name, protected FROM dolt_branches() WHERE name = 'main';
SELECT dolt_writer_mode('branch-only');

-- Branch-auto: a statement that writes on main first moves the session
-- to its writer branch, forked from main; the move is undone with its
-- transaction.
SELECT dolt_writer_mode('branch-auto');
SET dolt.writer = 'hank';
BEGIN;
INSERT INTO wm.t VALUES (20, 'hank');
SELECT active_branch();
ROLLBACK;
SELECT active_branch();
SELECT count(*) AS writer_branches FROM dolt_branches() WHERE kind = 'writer';
INSERT INTO wm.t VALUES (20, 'hank');
SELECT active_branch();
SELECT name, kind, base, owner, protected FROM dolt_branches()
WHERE name IN ('main', 'writer/hank/main') ORDER BY name;
UPDATE wm.t SET v = 'hank again' WHERE id = 20;
CREATE TABLE wm.hank (id int);
SELECT (dolt_integrate('-m', 'hank''s'))[2] AS fast_forward;
SELECT active_branch(), dolt_hashof('main') = dolt_hashof('HEAD') AS integrated;
-- (done with, it goes when the session leaves it)
SELECT dolt_checkout('main');
SELECT count(*) AS writer_branches FROM dolt_branches() WHERE kind = 'writer';
SELECT id, v FROM wm.t WHERE id = 20;
SELECT to_regclass('wm.hank') IS NOT NULL AS hank_table;
-- one with changes of its own stays; a write isn't moved there (its
-- definitions may not be main's); it is written there once checked out
INSERT INTO wm.t VALUES (21, 'hank');
SELECT dolt_checkout('main');
SELECT id FROM wm.t WHERE id = 21;
UPDATE wm.t SET v = 'hank, later' WHERE id = 20;
SELECT active_branch();
SELECT dolt_checkout('writer/hank/main');
UPDATE wm.t SET v = 'hank, later' WHERE id = 20;
SELECT id, v FROM wm.t WHERE id IN (20, 21) ORDER BY id;
SELECT dolt_reset('--hard');
-- what writes no branch's objects doesn't move: a role, a temporary table
SELECT dolt_checkout('main');
CREATE ROLE wm_nobody;
DROP ROLE wm_nobody;
CREATE TEMP TABLE wm_temp (id int);
SELECT active_branch();
-- a writer names its branches: a branch name's rules are its
SET dolt.writer = 'x.lock';
SET dolt.writer = 'a..b';
-- reads, and integrations, stay on main; a write a query makes is refused
SELECT dolt_checkout('main');
SELECT count(*) > 0 AS read FROM wm.t;
SELECT active_branch();
SELECT nextval('wm.s2');
-- writer branches' names are the database's; hank's, clean again, went
-- when the session left it
SELECT dolt_branch('writer/hank/other');
SELECT dolt_branch('-D', 'writer/hank/main');
SELECT count(*) AS writer_branches FROM dolt_branches() WHERE kind = 'writer';

-- Renamed, a protected branch stays protected, by its new name; only the
-- database's owner renames it, replaces it or moves it; a writer's branch
-- keeps its name.
SELECT dolt_branch('wm_pr');
SELECT dolt_branch('wm_pq');
SELECT dolt_branch('--protect', 'wm_pr');
SET ROLE wm_other;
SELECT dolt_branch('-m', 'wm_pr', 'wm_pr2');
SELECT dolt_branch('-m', '-f', 'wm_pq', 'wm_pr');
SELECT dolt_branch('-f', 'wm_pr', 'wm_pq');
RESET ROLE;
SELECT dolt_branch('-m', 'wm_pr', 'wm_pr2');
SELECT name, protected FROM dolt_branches() WHERE name LIKE 'wm_p%' ORDER BY name;
SELECT dolt_branch('-m', 'wm_pr2', 'writer/x/main');
SELECT dolt_branch('-m', 'writer/hank/other', 'wm_pr3');
SELECT dolt_branch('--unprotect', 'wm_pr2');
SELECT dolt_branch('-D', 'wm_pr2', 'wm_pq');

-- Only the database's owner sets the mode; and back to shared.
SET ROLE wm_other;
SELECT dolt_writer_mode('shared');
RESET ROLE;
SELECT dolt_writer_mode('nobody');
SELECT dolt_writer_mode('shared');
RESET dolt.writer;
REVOKE ALL ON wm.t FROM wm_other, wm_carol;
REVOKE USAGE ON SCHEMA wm FROM wm_other, wm_carol;
DROP ROLE wm_other;
DROP ROLE wm_carol;
