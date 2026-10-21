-- Commits of a database whose branches are copies of its versioned
-- schemas: heads and commits in the chunk store, each commit's root the
-- branch folded.

CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
INSERT INTO app.t VALUES (1, 'a');
SELECT length(dolt_commit('-m', 'first')::text) AS committed;
SELECT dolt_commit('-m', 'again');
INSERT INTO app.t VALUES (2, 'b');
SELECT length(dolt_commit('-m', 'second')::text) AS committed;
SELECT message, commit_order FROM dolt_log('main');
SELECT dolt_hashof('main~1') = (SELECT commit_hash FROM dolt_log('main') WHERE message = 'first') AS ancestry;
SELECT name, latest_commit_message, dirty FROM dolt_branches();

-- A commit goes with its transaction, and with its savepoint.
INSERT INTO app.t VALUES (3, 'c');
BEGIN;
SELECT length(dolt_commit('-m', 'rolled back')::text) AS committed;
ROLLBACK;
BEGIN;
SAVEPOINT s;
SELECT length(dolt_commit('-m', 'rolled back to a savepoint')::text) AS committed;
ROLLBACK TO s;
COMMIT;
SELECT message FROM dolt_log('main');
SELECT name, dirty FROM dolt_branches();
-- A refused empty commit records nothing, even where its error is caught
-- and the transaction goes on to commit.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT length(dolt_commit('-m', 'before empty')::text) AS committed;
SELECT sum(chunks) AS chunks FROM test_chunk_segments() \gset
BEGIN;
SAVEPOINT s;
SELECT dolt_commit('-m', 'empty, refused');
ROLLBACK TO s;
DO $$
BEGIN
	PERFORM dolt_commit('-m', 'empty, refused in a block');
EXCEPTION WHEN object_not_in_prerequisite_state THEN
	RAISE NOTICE 'refused: %', SQLERRM;
END $$;
COMMIT;
SELECT sum(chunks) = :chunks AS nothing_appended FROM test_chunk_segments();
SELECT message FROM dolt_log('main') LIMIT 1;
-- Emptiness is decided before anything is recorded, not undone after: the
-- refs refuse it without an error, and the head stays.
BEGIN;
SELECT dolt_hashof('main') AS head \gset
SELECT test_refs_commit('empty, refused by the refs', false) IS NULL AS refused;
SELECT dolt_hashof('main') = :'head' AS head_kept;
SELECT test_refs_commit('empty, allowed', true) IS NOT NULL AS committed;
SELECT message FROM dolt_log('main') LIMIT 2;
ROLLBACK;
-- and includes the transaction's own writes
BEGIN;
INSERT INTO app.t VALUES (4, 'd');
SELECT length(dolt_commit('-m', 'third')::text) AS committed;
COMMIT;
SELECT message FROM dolt_log('main');
SELECT name, dirty FROM dolt_branches();

-- A branch starts at its start point's head.
SELECT dolt_branch('f');
SELECT dolt_hashof('f') = dolt_hashof('main') AS same_head;
SELECT dolt_checkout('f');
UPDATE app.t SET v = 'B' WHERE id = 2;
SELECT length(dolt_commit('-m', 'on f')::text) AS committed;
SELECT dolt_hashof('HEAD') = dolt_hashof('f') AS head_is_f;
SELECT dolt_hashof('f~1') = dolt_hashof('main') AS parent_is_main;
SELECT dolt_branch('-c', 'f', 'g');
SELECT dolt_hashof('g') = dolt_hashof('f') AS copy_same_head;
SELECT dolt_checkout('main');
SELECT dolt_branch('-d', 'g');
SELECT dolt_branch('-d', 'f');		-- not merged into main
SELECT dolt_branch('-D', 'f', 'g');
SELECT name FROM dolt_branches();
SELECT dolt_hashof('f');

-- Commit roots depend only on content: the same rows by another history.
SELECT dolt_branch('h');
SELECT dolt_checkout('h');
DELETE FROM app.t;
INSERT INTO app.t VALUES (4, 'd'), (3, 'c'), (2, 'b'), (1, 'a');
SELECT length(dolt_commit('-m', 'same rows', '--allow-empty')::text) AS committed;
SELECT prolly_root_hash('h') = prolly_root_hash('main') AS same_root;
UPDATE app.t SET v = 'x' WHERE id = 1;
SELECT length(dolt_commit('-m', 'other rows')::text) AS committed;
SELECT prolly_root_hash('h') <> prolly_root_hash('main') AS other_root;

-- Bad refs and start points.
SELECT dolt_hashof('nope');
SELECT dolt_branch('k', 'nope~1');
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'h');

-- Dropping the last versioned schema is a change to commit: the history
-- stays the database's, and the next commit continues it.
SELECT count(*) AS commits FROM dolt_log('HEAD') \gset
SELECT dolt_hashof('HEAD') AS before_drop \gset
DROP SCHEMA app CASCADE;
SELECT count(*) = :commits AS history_kept FROM dolt_log('HEAD');
SELECT dolt_hashof('HEAD') = :'before_drop' AS same_head;
SELECT length(dolt_commit('-Am', 'drop the schema')::text) AS committed;
SELECT dolt_hashof('HEAD~1') = :'before_drop' AS continues_it;

-- A commit needs a message, and --amend isn't one of its options; its
-- author is dolt.author, else the session user with no email.
CREATE SCHEMA ca VERSIONED;
CREATE TABLE ca.t (id int PRIMARY KEY);
INSERT INTO ca.t VALUES (1);
SELECT dolt_commit('-A');
SELECT dolt_commit('-Am', 'ca', '--amend');
SET dolt.author = 'Ada Lovelace <ada@example.com>';
SELECT length(dolt_commit('-Am', 'by ada')::text) AS committed;
RESET dolt.author;
INSERT INTO ca.t VALUES (2);
SELECT length(dolt_commit('-am', 'by default')::text) AS committed;
SELECT message, committer = current_user AS session_user,
	CASE WHEN message = 'by ada' THEN committer END AS committer, email
  FROM dolt_log WHERE message IN ('by ada', 'by default') ORDER BY message;

-- A branch's name follows git's rules: one that ancestry steps, refspecs
-- or options would read otherwise is refused.
SELECT dolt_branch('bad~name');
SELECT dolt_branch('bad^name');
SELECT dolt_branch('bad:name');
SELECT dolt_branch('two words');
SELECT dolt_branch('a..b');
SELECT dolt_branch('HEAD');
SELECT dolt_branch('@');
SELECT dolt_branch('ends.lock');
SELECT dolt_branch('part.lock/b');
SELECT dolt_branch('.hidden');
SELECT dolt_branch('feat/.x');
SELECT dolt_branch('a//b');
SELECT dolt_branch('ends/');
SELECT dolt_branch('feature/ok-name_1.2');
SELECT dolt_hashof('feature/ok-name_1.2') = dolt_hashof('HEAD') AS resolves;
SELECT dolt_branch('-d', 'feature/ok-name_1.2');
-- As in Dolt: "HEAD" in any case is the head, not a branch; a name may
-- begin with "-", given after "--"; and one differing from a branch's
-- only by case names that branch, made, copied or renamed to.
SELECT dolt_branch('head');
SELECT dolt_hashof('head') = dolt_hashof('HEAD') AS head_any_case,
	dolt_hashof('Head~0') = dolt_hashof('HEAD') AS with_steps;
SELECT dolt_branch('--', '-dash');
SELECT name FROM dolt_branches WHERE name = '-dash';
-- (and named after "--" to the others taking a branch or ref)
SELECT dolt_checkout('-dash');
SELECT dolt_checkout('--', '-dash');
SELECT active_branch();
SELECT dolt_checkout('main');
SELECT (dolt_merge('--', '-dash'))[4];
SELECT dolt_reset('--hard', '--', '-dash');
SELECT dolt_cherry_pick('--', '-dash');
-- (and to a remote's, pushed, fetched and pulled)
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/dash-' || txid_current() AS url \gset
SELECT dolt_remote('add', 'dash', :'url');
SELECT dolt_push('dash', '--', '-dash') IS NOT NULL AS pushed;
SELECT dolt_fetch('dash', '--', '-dash');
SELECT name FROM dolt_remote_branches() WHERE name LIKE '%-dash';
SELECT dolt_pull('dash', '--', '-dash') IS NOT NULL AS pulled;
SELECT dolt_remote('remove', 'dash');
-- (a remote named like a flag, after "--": pulled from by name, and as
-- the session branch's upstream, set by -fu, short flags together)
SELECT dolt_remote('add', '--', '-r', :'url');
SELECT dolt_push('-fu', '--', '-r', 'main') IS NOT NULL AS pushed;
SELECT (dolt_pull('--', '-r', 'main'))[4];
SELECT (dolt_pull())[4];
SELECT dolt_remote('remove', '--', '-r');
SELECT dolt_gc('--', '--full');
SELECT dolt_branch('-D', '--', '-dash');
-- An option's value is the argument after it, "--" too (as in Dolt and
-- git), so "--" ends the options only where it isn't one.  "--" isn't a
-- branch's name (as in git), however it's given: no branch "--" is made.
SELECT dolt_checkout('-b', '--', 'main');
SELECT dolt_checkout('-b', '--');
SELECT dolt_branch('--', '--');
SELECT dolt_branch('-c', '--', 'main', '--');
SELECT dolt_checkout('--', '--');
SELECT name FROM dolt_branches WHERE name = '--';
SELECT active_branch();
BEGIN;
SELECT length(dolt_commit('--allow-empty', '-m', '--')::text) AS committed;
SELECT length(dolt_commit('--allow-empty', '-Am', '--', '--author', '--')::text) AS committed;
SELECT message, committer = '--' AS by_dashes FROM dolt_log('main') LIMIT 2;
SELECT dolt_branch('dashed');
SELECT dolt_checkout('dashed');
SELECT length(dolt_commit('--allow-empty', '-m', 'on dashed')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('--no-ff', '-m', '--', '--', 'dashed'))[4];
SELECT message FROM dolt_log('main') LIMIT 1;
ROLLBACK;
-- A lone "--" ends the options of the rest too, with nothing after it.
SELECT dolt_add('--');
SELECT dolt_verify_constraints('--');
-- -b's start point is the name given apart from it, before -b or after
-- (as in Dolt and git); -b is given once.
SELECT dolt_branch('cko_old', 'HEAD~1');
SELECT dolt_checkout('cko_old', '-b', 'cko_new');
SELECT dolt_hashof('cko_new') = dolt_hashof('cko_old') AS at_old,
	dolt_hashof('cko_new') <> dolt_hashof('main') AS not_main;
SELECT dolt_checkout('main');
SELECT dolt_checkout('-b', 'cko_x', 'cko_old', '-b', 'cko_y');
SELECT dolt_checkout('-b', 'cko_x', 'cko_old', 'main');
SELECT name FROM dolt_branches WHERE name IN ('cko_x', 'cko_y');
SELECT dolt_branch('-D', 'cko_old', 'cko_new');
SELECT dolt_branch('cased');
SELECT dolt_branch('CASED');
SELECT dolt_branch('-f', 'Cased');
SELECT dolt_checkout('-b', 'CASED');
SELECT dolt_branch('-c', 'cased', 'CASED');
SELECT dolt_branch('other');
SELECT dolt_branch('-m', 'other', 'CASED');
SELECT dolt_branch('-mf', 'other', 'CASED');
SELECT name FROM dolt_branches WHERE lower(name) = 'cased';
-- (a branch renamed to its own name in another case: still one)
SELECT dolt_branch('-m', 'cased', 'Cased');
SELECT dolt_branch('-m', 'Cased', 'cased');
SELECT name FROM dolt_branches WHERE lower(name) = 'cased';
-- Forced, a branch made or copied takes the place of one of its name
-- (not the session's own).
SELECT dolt_hashof('cased') AS cased_at \gset
INSERT INTO ca.t VALUES (3);
SELECT length(dolt_commit('-am', 'moved on')::text) AS committed;
SELECT dolt_branch('cased');
SELECT dolt_branch('-f', 'cased');
SELECT dolt_hashof('cased') = dolt_hashof('HEAD') AS forced_to_head,
	dolt_hashof('cased') <> :'cased_at' AS moved;
SELECT dolt_branch('-cf', 'other', 'cased');
SELECT dolt_hashof('cased') = dolt_hashof('other') AS copied;
-- moved, not made again: to a start point naming the branch itself, its
-- uncommitted changes dropped; main, from another branch
SELECT dolt_hashof('cased~1') AS back \gset
SELECT dolt_branch('-f', 'cased', 'cased~1');
SELECT dolt_hashof('cased') = :'back' AS moved_back;
SELECT dolt_branch('-cf', 'cased', 'cased');
SELECT dolt_checkout('cased');
INSERT INTO ca.t VALUES (4);
SELECT dolt_checkout('main');
SELECT dolt_branch('-f', 'cased', 'main');
SELECT dolt_checkout('cased');
SELECT id FROM ca.t ORDER BY id;
SELECT dolt_hashof('main') AS main_at \gset
SELECT dolt_branch('-f', 'main', 'main~1');
SELECT dolt_hashof('main') <> :'main_at' AS main_moved;
SELECT dolt_branch('-f', 'main', :'main_at');
SELECT dolt_checkout('main');
SELECT dolt_branch('-f', 'main');
SELECT dolt_branch('-D', 'cased', 'other');
DROP SCHEMA ca CASCADE;
SELECT length(dolt_commit('-Am', 'ca gone')::text) AS committed;

-- A cherry-pick resolved to nothing is refused as empty, by the refs
-- before they record anything: the pick stays in progress, to be aborted.
CREATE SCHEMA cp VERSIONED;
CREATE TABLE cp.t (id int PRIMARY KEY, v text);
INSERT INTO cp.t VALUES (1, 'base');
SELECT length(dolt_commit('-Am', 'cp base')::text) AS committed;
SELECT dolt_branch('cp_src');
SELECT dolt_checkout('cp_src');
UPDATE cp.t SET v = 'src' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'cp src')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE cp.t SET v = 'main' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'cp main')::text) AS committed;
BEGIN;
SELECT (dolt_cherry_pick('cp_src'))[2] AS conflicts;
SELECT dolt_conflicts_resolve('--ours', 'cp.t');
SELECT test_refs_commit('cp resolved to nothing', false) IS NULL AS refused;
SELECT dolt_cherry_pick('--abort') IS NOT NULL AS aborted;
SELECT v FROM cp.t;
ROLLBACK;
SELECT dolt_branch('-D', 'cp_src');
DROP SCHEMA cp CASCADE;
SELECT length(dolt_commit('-Am', 'cp gone')::text) AS committed;

-- dolt_commits: every commit any branch reaches; dolt_commit_ancestors:
-- each one's parents, in order, a root commit's null (#588).
CREATE SCHEMA cg VERSIONED;
CREATE TABLE cg.t (id int PRIMARY KEY, message text);
INSERT INTO cg.t VALUES (1, 'test1');
SELECT length(dolt_commit('-Am', 'cg 1')::text) AS committed;
SELECT dolt_branch('cg_side');
SELECT dolt_checkout('cg_side');
UPDATE cg.t SET message = 'test2' WHERE id = 1;
SELECT length(dolt_commit('-am', 'cg 2')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO cg.t VALUES (2, 'test3');
SELECT length(dolt_commit('-am', 'cg 3')::text) AS committed;
-- (a commit on a branch main doesn't reach)
SELECT message FROM dolt_commits WHERE message LIKE 'cg _' ORDER BY message;
SELECT count(*) FILTER (WHERE message = 'cg 2') AS in_log FROM dolt_log;
SELECT (SELECT count(*) FROM dolt_commits) =
	(SELECT count(*) FROM (SELECT commit_hash FROM dolt_log('main')
						   UNION SELECT commit_hash FROM dolt_log('cg_side')) l)
	AS every_branch;
SELECT count(DISTINCT commit_hash) = count(*) AS once FROM dolt_commits;
-- (a merge commit's two parents, ours first)
SELECT (dolt_merge('--no-ff', '-m', 'cg merged', 'cg_side'))[1] IS NOT NULL AS merged;
SELECT parent_index FROM dolt_commit_ancestors
	WHERE commit_hash = dolt_hashof('HEAD') ORDER BY parent_index;
SELECT a.parent_index,
	(SELECT message FROM dolt_log WHERE commit_hash = a.parent_hash) AS message
	FROM dolt_commit_ancestors a
	WHERE a.commit_hash = dolt_hashof('HEAD') ORDER BY a.parent_index;
-- (the root commit's parent is null; every other commit's is a commit)
SELECT count(*) AS roots, min(parent_index) AS parent_index
	FROM dolt_commit_ancestors WHERE parent_hash IS NULL;
SELECT count(*) AS dangling FROM dolt_commit_ancestors a
	WHERE a.parent_hash IS NOT NULL
	AND NOT EXISTS (SELECT 1 FROM dolt_commits c WHERE c.commit_hash = a.parent_hash);
SELECT count(*) AS without_row FROM dolt_commits c
	WHERE NOT EXISTS (SELECT 1 FROM dolt_commit_ancestors a
					  WHERE a.commit_hash = c.commit_hash);
-- (a commit only a deleted branch reached is gone)
SELECT dolt_branch('cg_gone');
SELECT dolt_checkout('cg_gone');
UPDATE cg.t SET message = 'gone' WHERE id = 1;
SELECT length(dolt_commit('-am', 'cg gone soon')::text) AS committed;
SELECT dolt_hashof('cg_gone') AS cg_gone_at \gset
SELECT dolt_checkout('main');
SELECT count(*) AS on_cg_gone FROM dolt_commits WHERE message = 'cg gone soon';
SELECT dolt_branch('-D', 'cg_gone', 'cg_side');
SELECT count(*) AS on_cg_gone FROM dolt_commits WHERE message = 'cg gone soon';
-- (and stays gone with a session checked out at it: the hidden branch
-- that holds it there is Dolt's detached HEAD, not a branch)
SELECT dolt_checkout(:'cg_gone_at') IS NOT NULL AS detached;
SELECT count(*) AS on_cg_gone FROM dolt_commits WHERE message = 'cg gone soon';
SELECT count(*) AS on_cg_gone FROM dolt_commit_ancestors
	WHERE commit_hash = :'cg_gone_at';
SELECT dolt_checkout('main');
-- (a writer's branch is a branch: its commits are listed, though main's
-- log doesn't reach them)
SELECT dolt_writer_mode('branch-auto');
SET dolt.writer = 'cgw';
SET client_min_messages = warning;
INSERT INTO cg.t VALUES (3, 'writer');
RESET client_min_messages;
SELECT active_branch();
SELECT length(dolt_commit('-am', 'cg on a writer branch')::text) AS committed;
SELECT dolt_checkout('main');
SELECT count(*) AS on_writer FROM dolt_commits
	WHERE message = 'cg on a writer branch';
SELECT count(*) AS on_writer FROM dolt_commit_ancestors
	WHERE commit_hash = dolt_hashof('writer/cgw/main');
SELECT count(*) AS in_log FROM dolt_log WHERE message = 'cg on a writer branch';
SELECT dolt_branch('-D', 'writer/cgw/main');
RESET dolt.writer;
SELECT dolt_writer_mode('shared');
DROP SCHEMA cg CASCADE;
SELECT length(dolt_commit('-Am', 'cg dropped')::text) AS committed;
