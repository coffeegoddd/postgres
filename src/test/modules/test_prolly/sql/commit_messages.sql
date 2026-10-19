-- A commit's message and author, wherever they're given: neither is empty
-- (as Dolt's commits refuse an empty message, author name or email).  An
-- author is "Name <email>", or a name alone, as the session user is.
CREATE SCHEMA cm VERSIONED;
CREATE TABLE cm.t (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'cm base')::text) AS committed;
SELECT dolt_hashof('HEAD') AS base \gset
INSERT INTO cm.t VALUES (1);

-- dolt_commit: an empty message, an empty author, an author without a
-- name or with an empty email.  (--allow-empty: each one would commit.)
SELECT dolt_commit('-Am', '');
SELECT dolt_commit('--allow-empty', '--message', '');
SELECT dolt_commit('--allow-empty', '-Am', 'by nobody', '--author', '');
SELECT dolt_commit('--allow-empty', '-Am', 'by nobody', '--author', ' <ada@example.com>');
SELECT dolt_commit('--allow-empty', '-Am', 'by nobody', '--author', 'Ada <>');
SELECT dolt_hashof('HEAD') = :'base' AS nothing_committed;

-- An author with an email, or a name alone.
SELECT length(dolt_commit('-Am', 'by ada', '--author', 'Ada <ada@example.com>')::text) AS committed;
INSERT INTO cm.t VALUES (2);
SELECT length(dolt_commit('-Am', 'by bob', '--author', 'Bob')::text) AS committed;
SELECT committer, email, message FROM dolt_log('main') LIMIT 2;

-- dolt.author: empty is the session user; otherwise an author too.
SET dolt.author = ' <ada@example.com>';
SET dolt.author = 'Ada <>';
SET dolt.author = 'Ada <ada@example.com>';
RESET dolt.author;

-- dolt_merge's and dolt_pull's -m, dolt_integrate's, dolt_revert's
-- --author.
SELECT dolt_branch('cm_f');
SELECT dolt_checkout('cm_f');
INSERT INTO cm.t VALUES (3);
SELECT length(dolt_commit('-Am', 'on cm_f')::text) AS committed;
SELECT dolt_checkout('main');
SELECT dolt_hashof('HEAD') AS before_merge \gset
SELECT dolt_merge('cm_f', '-m', '');
SELECT dolt_merge('--no-ff', '--message', '', 'cm_f');
SELECT dolt_pull('-m', '', 'origin', 'main');
SELECT dolt_integrate('main', '-m', '');
SELECT dolt_revert('--author', '', 'HEAD');
SELECT dolt_revert('--author', 'Ada <>', 'HEAD');
SELECT dolt_hashof('HEAD') = :'before_merge' AS nothing_merged;
SELECT r[2:4] FROM dolt_revert('--author', 'Ada <ada@example.com>', 'HEAD') r;
SELECT committer, email, message FROM dolt_log('main') LIMIT 1;

-- An abort makes no commit: dolt_merge's -m and dolt_revert's --author
-- are ignored with --abort (as Dolt ignores -m), even empty.
CREATE TABLE cm.c (id int PRIMARY KEY, v text);
INSERT INTO cm.c VALUES (1, 'base');
SELECT length(dolt_commit('-Am', 'cm.c')::text) AS committed;
SELECT dolt_checkout('cm_f');
SELECT dolt_merge('main') IS NOT NULL AS merged;
UPDATE cm.c SET v = 'theirs';
SELECT length(dolt_commit('-am', 'theirs')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE cm.c SET v = 'ours';
SELECT length(dolt_commit('-am', 'ours')::text) AS committed;
SELECT dolt_hashof('HEAD') AS before_abort \gset
BEGIN;
SELECT (dolt_merge('cm_f'))[2:4] AS conflicted;
SELECT (dolt_merge('--abort', '-m', ''))[2:4] AS aborted;
COMMIT;
SELECT dolt_hashof('HEAD') = :'before_abort' AS back, v FROM cm.c;
SELECT dolt_revert('--abort', '--author', '');	-- (no revert to abort)

-- (a transaction's commit message isn't empty either)
SET dolt.transaction_commit_message = '';

SELECT dolt_branch('-D', 'cm_f');
DROP SCHEMA cm CASCADE;
SELECT length(dolt_commit('-Am', 'cm gone')::text) AS committed;
