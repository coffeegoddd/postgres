-- Version control's long operations cancelled partway, on the history
-- store: each op's chunk reads counted in a run rolled back, it is
-- cancelled, in a savepoint, as it reads its last, after it has written
-- (the next check for interrupts ends it); rolled back to the savepoint,
-- the repository is as it was, and no lock of it is left.  Then, not
-- cancelled, each runs to its end.
CREATE EXTENSION IF NOT EXISTS test_prolly;
SELECT test_vc_open();
SELECT test_vc_commit('main', 'base');
SELECT test_vc_commit('main', 'c' || i) FROM generate_series(1, 4) i;
SELECT test_vc_branch('b', 'main');
SELECT test_vc_commit('b', 'b' || i) FROM generate_series(1, 3) i;
SELECT test_vc_commit('main', 'c5');

CREATE TEMP TABLE repo_was AS
	SELECT test_vc_log('main') AS main, test_vc_log('b') AS b,
		(SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'vc/%') AS refs;
CREATE FUNCTION repo_same() RETURNS bool LANGUAGE sql AS $$
	SELECT test_vc_log('main') = main AND test_vc_log('b') = b AND
		(SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'vc/%') = refs
	FROM repo_was
$$;
-- (locks this backend holds, but on relations, its virtual xid, and its
-- transaction's own xid: a row's, an object's, a subtransaction's)
CREATE FUNCTION locks_left() RETURNS int8 LANGUAGE sql AS $$
	SELECT count(*) FROM pg_locks
	WHERE pid = pg_backend_pid() AND locktype NOT IN ('virtualxid', 'relation') AND
		NOT (locktype = 'transactionid' AND
			 transactionid::text = pg_current_xact_id_if_assigned()::text)
$$;

-- the diff, which writes nothing: cancelled mid-walk (at its last read
-- it has no check for interrupts left to make)
BEGIN;
SELECT test_vc_cancel('diff', 0) AS reads \gset
ROLLBACK;
BEGIN;
SAVEPOINT s;
SELECT test_vc_cancel('diff', :reads - 1);
ROLLBACK TO s;
SELECT repo_same(), locks_left();
COMMIT;

-- the merge: cancelled as it reads its last chunk, having written
BEGIN;
SELECT test_vc_cancel('merge', 0) AS reads \gset
ROLLBACK;
BEGIN;
SAVEPOINT s;
SELECT test_vc_cancel('merge', :reads);
ROLLBACK TO s;
SELECT test_vc_cancel_writes() > 0 AS had_written, repo_same(), locks_left();
COMMIT;

-- the sever, keeping its history
BEGIN;
SELECT test_vc_cancel('sever', 0) AS reads \gset
ROLLBACK;
BEGIN;
SAVEPOINT s;
SELECT test_vc_cancel('sever', :reads);
ROLLBACK TO s;
SELECT test_vc_cancel_writes() > 0 AS had_written, repo_same(), locks_left();
COMMIT;

-- the sever, rewriting it
SELECT test_vc_rewrite(true);
DELETE FROM repo_was;
INSERT INTO repo_was
	SELECT test_vc_log('main'), test_vc_log('b'),
		(SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'vc/%');
BEGIN;
SELECT test_vc_cancel('sever', 0) AS reads \gset
ROLLBACK;
BEGIN;
SAVEPOINT s;
SELECT test_vc_cancel('sever', :reads);
ROLLBACK TO s;
SELECT test_vc_cancel_writes() > 0 AS had_written, repo_same(), locks_left();
COMMIT;

-- and each runs to its end, nothing of the cancelled ones in its way
SELECT test_vc_rewrite(false);
SELECT test_vc_cancel('diff', 0) > 0 AS diffed;
SELECT test_vc_cancel('merge', 0) > 0 AS merged;
SELECT test_vc_log('main');
SELECT test_vc_cancel('sever', 0) > 0 AS severed;
SELECT test_vc_head_in_window('main') AS kept,
	(SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'vc/segment/%') AS segments;

-- transfers lock the receiver's state and read what committed while they
-- waited: a READ COMMITTED transaction's, refused in one with a snapshot
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT test_vc_transfer('push');
ROLLBACK;
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT test_vc_transfer('fetch');
ROLLBACK;
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT test_vc_transfer('deepen');
ROLLBACK;
SELECT test_vc_transfer('push') AS push, test_vc_transfer('fetch') AS fetch;

SELECT test_vc_clear() > 0 AS cleared;
DROP FUNCTION repo_same(), locks_left();
