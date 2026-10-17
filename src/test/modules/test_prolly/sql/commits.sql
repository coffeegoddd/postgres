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
SELECT dolt_branch('-flag');
SELECT dolt_branch('HEAD');
SELECT dolt_branch('ends.lock');
SELECT dolt_branch('feature/ok-name_1.2');
SELECT dolt_hashof('feature/ok-name_1.2') = dolt_hashof('HEAD') AS resolves;
SELECT dolt_branch('-d', 'feature/ok-name_1.2');
DROP SCHEMA ca CASCADE;
SELECT length(dolt_commit('-Am', 'ca gone')::text) AS committed;
