-- Version control's long operations cancelled partway, on the history
-- store (as each reads its second chunk): each stops at its next check for
-- interrupts, and leaves nothing behind (the repository as it was, no
-- lock); then, not cancelled, each runs to its end.
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
-- (the locks a statement of this backend holds, but for its own)
CREATE FUNCTION locks_left() RETURNS int8 LANGUAGE sql AS $$
	SELECT count(*) FROM pg_locks
	WHERE pid = pg_backend_pid() AND locktype NOT IN ('virtualxid', 'relation')
$$;

SELECT test_vc_cancel('diff', 2);
SELECT repo_same(), locks_left();
SELECT test_vc_cancel('merge', 2);
SELECT repo_same(), locks_left();
SELECT test_vc_cancel('sever', 2);
SELECT repo_same(), locks_left();
SELECT test_vc_rewrite(true);
DELETE FROM repo_was;
INSERT INTO repo_was
	SELECT test_vc_log('main'), test_vc_log('b'),
		(SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'vc/%');
SELECT test_vc_cancel('sever', 2);
SELECT repo_same(), locks_left();

-- and each runs to its end, nothing of the cancelled ones in its way
SELECT test_vc_rewrite(false);
SELECT test_vc_cancel('diff', 1000000);
SELECT test_vc_cancel('merge', 1000000);
SELECT test_vc_log('main');
SELECT test_vc_cancel('sever', 1000000);
SELECT test_vc_head_in_window('main') AS kept,
	(SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'vc/segment/%') AS segments;

SELECT test_vc_clear() > 0 AS cleared;
DROP FUNCTION repo_same(), locks_left();
