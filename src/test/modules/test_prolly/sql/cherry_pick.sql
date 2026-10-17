-- Cherry-picks and reverts on branches made by copying: a commit's
-- changes from its parent, merged three-way into the branch and committed.

CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
CREATE TABLE app.k (a text);
INSERT INTO app.t SELECT g, 'v' || g FROM generate_series(1, 10) g;
INSERT INTO app.k VALUES ('x');
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('feature');
SELECT dolt_checkout('feature');
UPDATE app.t SET v = 'one' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'feature one')::text) AS committed;
INSERT INTO app.t VALUES (11, 'eleven');
DELETE FROM app.t WHERE id = 2;
INSERT INTO app.k VALUES ('x');
SELECT (dolt_commit('-Am', 'feature two'))[1] AS two \gset
SELECT dolt_checkout('main');
UPDATE app.t SET v = 'main' WHERE id = 5;
SELECT length(dolt_commit('-Am', 'on main')::text) AS committed;

-- Only the picked commit's changes, not its parent's.
SELECT (dolt_cherry_pick(:'two'))[2:4];
SELECT id, v FROM app.t WHERE id IN (1, 2, 5, 11) ORDER BY id;
SELECT a, count(*) FROM app.k GROUP BY a;
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;
SELECT dolt_hashof('HEAD^2');		-- one parent
SELECT * FROM dolt_status();

-- Reverting it undoes them.
SELECT dolt_revert('HEAD');
SELECT id, v FROM app.t WHERE id IN (1, 2, 5, 11) ORDER BY id;
SELECT a, count(*) FROM app.k GROUP BY a;
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;

-- A conflict outside a transaction block rolls the pick back; so are
-- uncommitted changes it would change refused, and merges.
UPDATE app.t SET v = 'main one' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'main one')::text) AS committed;
SELECT dolt_cherry_pick('feature~1');

-- In a transaction block, a conflict leaves the pick in progress, to
-- resolve and commit: with one parent, and the picked commit's message
-- (--continue) or one's own.
BEGIN;
SELECT (dolt_cherry_pick('feature~1'))[2:4];
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::app.t);
SAVEPOINT s;
SELECT dolt_merge('feature');
ROLLBACK TO s;
SELECT dolt_conflicts_resolve('--theirs', 'app.t');
SELECT length((dolt_cherry_pick('--continue'))[1]) AS picked;
COMMIT;
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;
SELECT id, v FROM app.t WHERE id = 1;
SELECT dolt_hashof('HEAD^2');
SELECT dolt_reset('--hard', 'HEAD~1');
BEGIN;
SELECT (dolt_cherry_pick('feature~1'))[2:4];
SELECT dolt_conflicts_resolve('--ours', 'app.t');
SELECT length(dolt_commit('--allow-empty', '-am', 'picked, ours kept')::text) AS committed;
COMMIT;
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;
SELECT dolt_hashof('HEAD^2');
SELECT dolt_reset('--hard', 'HEAD~1');
-- (--continue after ours kept: nothing to commit, unless allowed)
BEGIN;
SELECT (dolt_cherry_pick('feature~1'))[2:4];
SELECT dolt_conflicts_resolve('--ours', 'app.t');
SAVEPOINT s;
SELECT dolt_cherry_pick('--continue');
ROLLBACK TO s;
SELECT length((dolt_cherry_pick('--allow-empty', '--continue'))[1]) AS picked;
COMMIT;
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;
SELECT dolt_reset('--hard', 'HEAD~1');
-- one kept with its conflicts is aborted
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_cherry_pick('feature~1'))[2:4];
RESET dolt.allow_commit_conflicts;
SELECT (dolt_cherry_pick('--abort'))[1:2];
SELECT id, v FROM app.t WHERE id = 1;
SELECT dolt_cherry_pick('--continue');
SELECT dolt_cherry_pick('--continue', '--abort');
-- (a pick that conflicts isn't made over uncommitted changes, which its
-- commit would take in and its abort lose)
UPDATE app.k SET a = 'uncommitted';
BEGIN;
SELECT (dolt_cherry_pick('feature~1'))[2:4];
ROLLBACK;
SELECT a FROM app.k;
SELECT dolt_reset('--hard');
UPDATE app.t SET v = 'uncommitted' WHERE id = 3;
SELECT dolt_cherry_pick(:'two');
SELECT dolt_reset('--hard');
SELECT dolt_merge('feature', '-m', 'merged');
SELECT dolt_conflicts_resolve('--theirs', '.');
BEGIN;
SELECT (dolt_merge('feature'))[2:4];
SELECT dolt_conflicts_resolve('--theirs', '.');
SELECT length(dolt_commit('-Am', 'merged')::text) AS committed;
COMMIT;
SELECT dolt_cherry_pick('HEAD');		-- a merge commit
SELECT dolt_revert('nope');

-- Several reverts are one commit, every ref named as the branch was
-- before: HEAD~1 is r1, not the commit HEAD's revert makes.
UPDATE app.t SET v = 'r1' WHERE id = 6;
SELECT length(dolt_commit('-am', 'r1')::text) AS committed;
UPDATE app.t SET v = 'r2' WHERE id = 7;
SELECT length(dolt_commit('-am', 'r2')::text) AS committed;
SELECT dolt_hashof('HEAD~2') AS before_r \gset
SELECT dolt_revert('HEAD', 'HEAD~1');
SELECT id, v FROM app.t WHERE id IN (6, 7) ORDER BY id;
SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;
SELECT dolt_hashof('HEAD~3') = :'before_r' AS one_commit;

-- Picking what the branch has already changes nothing: refused, unless
-- --allow-empty.  A pick keeps its commit's author.
UPDATE app.t SET v = 'e1' WHERE id = 8;
SELECT (dolt_commit('-am', 'e1'))[1] AS e1 \gset
SELECT dolt_cherry_pick(:'e1');
SELECT (dolt_cherry_pick('--allow-empty', :'e1'))[2:4];
SELECT dolt_hashof('HEAD^') = :'e1' AS on_e1;
SELECT dolt_branch('cp_author');
SELECT dolt_checkout('cp_author');
UPDATE app.t SET v = 'ada' WHERE id = 9;
SELECT length(dolt_commit('-am', 'by ada', '--author', 'Ada <ada@example.com>')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_cherry_pick('cp_author'))[2:4];
SELECT committer, email, message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1;

-- Refusals name what was refused; a branch's first commit has no
-- changes from a parent to undo.
UPDATE app.t SET v = 'uncommitted' WHERE id = 3;
SELECT dolt_revert('HEAD');
SELECT dolt_reset('--hard');
SELECT commit_hash AS first FROM dolt_log('main') ORDER BY commit_order LIMIT 1 \gset
\set VERBOSITY terse
SELECT dolt_revert(:'first');
\set VERBOSITY default

-- Nor is a pick or revert made while a merge is in progress.
SELECT dolt_branch('cp_c');
SELECT dolt_checkout('cp_c');
UPDATE app.t SET v = 'c' WHERE id = 4;
SELECT length(dolt_commit('-am', 'c')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE app.t SET v = 'm' WHERE id = 4;
SELECT length(dolt_commit('-am', 'm')::text) AS committed;
BEGIN;
SELECT (dolt_merge('cp_c'))[2:4];
SAVEPOINT s;
SELECT dolt_cherry_pick('cp_author');
ROLLBACK TO s;
SELECT dolt_revert('HEAD');
ROLLBACK;
SELECT dolt_branch('-D', 'cp_c');
SELECT dolt_branch('-D', 'cp_author');

SELECT dolt_branch('-D', 'feature');
DROP SCHEMA app CASCADE;
