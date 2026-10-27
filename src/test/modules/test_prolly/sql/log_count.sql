-- A branch's kept commit (the pages kept as of it, for its window, as a
-- head moves there) records the commits it reaches, itself included (one
-- more than its first parent's, and for a merge, more by those its other
-- parents reach that the first doesn't), as Dolt's commit closure counts
-- them, outside the commit: commit ids are as they were.  So count(*) of
-- dolt_log(ref) alone is planned as a scan giving that count, reading no
-- history; a history horizon, or a commit no branch keeps with a count,
-- has the log counted as it gives its rows.
SELECT current_database() AS home \gset

-- A commit's id is its content's alone, as before counts were kept: a
-- first commit of fixed content, author and date has the id it had.
CREATE DATABASE regress_log_count_ids;
\c regress_log_count_ids
CREATE SCHEMA ids VERSIONED;
CREATE TABLE ids.t (id int PRIMARY KEY);
INSERT INTO ids.t VALUES (1);
SET dolt.author = 'pinned <pinned@example.com>';
SELECT dolt_commit('-Am', 'pinned', '--date', '2001-02-03 04:05:06+00') AS pinned_id;
\c :home
DROP DATABASE regress_log_count_ids;
CREATE DATABASE regress_log_count;
\c regress_log_count
CREATE EXTENSION test_prolly;
CREATE FUNCTION commits_read() RETURNS bigint LANGUAGE sql AS $$
	SELECT coalesce((SELECT value FROM pg_stat_prolly WHERE name = 'commits_read'), 0)
$$;
\set flush 'SELECT pg_stat_force_next_flush() IS NULL AS flushed \\gset'
-- (the count planned, and the log's rows counted: the same)
CREATE FUNCTION counts(ref text) RETURNS text LANGUAGE sql AS $$
	SELECT (SELECT count(*) FROM dolt_log(ref)) || ' ' ||
		(SELECT count(*) FROM (SELECT * FROM dolt_log(ref) OFFSET 0) l)
$$;

-- HEAD before a first commit: nothing
SELECT count(*) AS none_yet FROM dolt_log;

CREATE SCHEMA lc VERSIONED;
CREATE TABLE lc.t (id int PRIMARY KEY, v int);
INSERT INTO lc.t VALUES (1, 0);
CREATE FUNCTION mk(n int, k int DEFAULT 1) RETURNS int LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..n LOOP
		UPDATE lc.t SET v = v + 1 WHERE id = k;
		PERFORM dolt_commit('-Am', 'c' || i);
	END LOOP;
	RETURN n;
END $$;
SELECT mk(40) AS commits;

-- the plan: the count, not the log's rows
EXPLAIN (COSTS OFF) SELECT count(*) FROM dolt_log;
EXPLAIN (COSTS OFF) SELECT count(*) FROM dolt_log('main');
-- (only count(*) of the log alone: a qual, a GROUP BY, another aggregate,
-- a join read the log)
EXPLAIN (COSTS OFF) SELECT count(*) FROM dolt_log WHERE message LIKE 'c1%';
EXPLAIN (COSTS OFF) SELECT committer, count(*) FROM dolt_log GROUP BY committer;
EXPLAIN (COSTS OFF) SELECT count(*), max(date) FROM dolt_log;
EXPLAIN (COSTS OFF) SELECT count(message) FROM dolt_log;
EXPLAIN (COSTS OFF) SELECT count(*) FROM dolt_log l, dolt_log m
	WHERE l.commit_hash = m.commit_hash;
SELECT counts('HEAD') AS head, counts('main~10') AS ancestor;
-- count(*) in expressions, twice, ordered, limited
SELECT count(*) + 1 AS plus_one, count(*) * 2 AS twice FROM dolt_log;
SELECT count(*) AS n FROM dolt_log ORDER BY 1 LIMIT 1;
-- a ref the count computes as it runs: a parameter, a correlated one
PREPARE cnt(text) AS SELECT count(*) FROM dolt_log($1);
EXECUTE cnt('main~5');
SELECT r, (SELECT count(*) FROM dolt_log(r)) AS commits
FROM (VALUES ('main'), ('main~1'), ('main~39')) v(r) ORDER BY r;
SELECT count(*) AS none FROM dolt_log(NULL);
SELECT count(*) FROM dolt_log('no_such_ref');

-- the count is dolt_log's: one who can't execute it can't count it
CREATE ROLE regress_log_count_nobody;
REVOKE EXECUTE ON FUNCTION dolt_log(text) FROM PUBLIC;
SET ROLE regress_log_count_nobody;
SELECT count(*) FROM dolt_log;
RESET ROLE;
GRANT EXECUTE ON FUNCTION dolt_log(text) TO PUBLIC;
DROP ROLE regress_log_count_nobody;

-- the count reads the head's commit, not the history
:flush
SELECT commits_read() AS before \gset
SELECT count(*) AS commits FROM dolt_log;
:flush
SELECT commits_read() - :before <= 2 AS read_one;

-- Merges: the commits both sides reach counted once.  A side branch, a
-- merge, the side going on and merged again (criss-cross: each side
-- merged into the other), and a merge of a branch behind.
SELECT dolt_branch('a');
SELECT dolt_branch('b');
SELECT dolt_checkout('a');
INSERT INTO lc.t VALUES (2, 0);
SELECT mk(5, 2) AS commits_a;
SELECT dolt_checkout('b');
INSERT INTO lc.t VALUES (3, 0);
SELECT mk(7, 3) AS commits_b;
SELECT dolt_checkout('a');
SELECT dolt_merge('b', '--no-ff', '-m', 'b into a') IS NOT NULL AS merged;
SELECT counts('a') AS a_merged, test_commit_commits(dolt_hashof('a')) AS recorded;
SELECT dolt_checkout('b');
INSERT INTO lc.t VALUES (4, 0);
SELECT mk(2, 4) AS more_b;
SELECT dolt_merge('a~1', '--no-ff', '-m', 'a~1 into b') IS NOT NULL AS merged;
SELECT dolt_checkout('a');
INSERT INTO lc.t VALUES (5, 0);
SELECT mk(3, 5) AS more_a;
SELECT dolt_merge('b', '--no-ff', '-m', 'b into a again') IS NOT NULL AS merged;
SELECT counts('a') AS a, counts('b') AS b, counts('a~1') AS a_parent;
SELECT dolt_checkout('main');
SELECT dolt_merge('a', '--no-ff', '-m', 'a into main') IS NOT NULL AS merged;
SELECT counts('main') AS main;
-- a commit reached by both sides at once: each counted once
SELECT count(DISTINCT commit_hash) = (SELECT count(*) FROM dolt_log) AS once
FROM (SELECT * FROM dolt_log OFFSET 0) l;


-- Commits no branch keeps (made elsewhere): counted as the log is; once
-- a head moves to one, it is kept, its count from the nearest kept first
-- parent a few below; the commits between keep none.
SELECT test_commit_chain(dolt_hashof('main'), 3) AS old_head \gset
SELECT test_commit_commits(:'old_head') AS kept_for_old;
:flush
SELECT commits_read() AS before \gset
SELECT counts(:'old_head') AS counted;
:flush
SELECT commits_read() - :before > 50 AS walked;
SELECT dolt_reset('--hard', :'old_head') IS NOT NULL AS reset;
SELECT test_commit_commits(:'old_head') AS kept_once_moved;
SELECT test_commit_commits(dolt_hashof('main~1')) AS kept_beneath;
UPDATE lc.t SET v = 0 WHERE id = 1;
SELECT length(dolt_commit('-Am', 'on old')::text) AS committed;
SELECT test_commit_commits(dolt_hashof('main')) AS kept_on_new,
	counts('main') AS counted;

-- A commit whose root is let go of (out of the window) keeps no count:
-- the log counted, the same; the head's is kept always.
ALTER DATABASE regress_log_count SET prolly.history_window = '1 microsecond';
SELECT test_retain_roots('main');
SELECT test_commit_commits(dolt_hashof('main~3')) AS let_go,
	counts('main~3') AS counted;
SELECT test_commit_commits(dolt_hashof('main')) AS head_kept,
	counts('main') AS counted;
ALTER DATABASE regress_log_count RESET prolly.history_window;

-- A clone is sent none (they are the refs', not the commits'): its head's keep
-- records one, the same count.
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/log-count-' ||
	txid_current() AS url \gset
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_push('origin', 'main') AS pushed;
SELECT counts('main') AS main_counts \gset
CREATE DATABASE regress_log_count_clone;
\c regress_log_count_clone
SELECT dolt_clone(:'url') IS NOT NULL AS cloned;
SELECT (SELECT count(*) FROM dolt_log('main')) || ' ' ||
	(SELECT count(*) FROM (SELECT * FROM dolt_log('main') OFFSET 0) l) =
	:'main_counts' AS cloned_counts;
\c :home
DROP DATABASE regress_log_count_clone;
DROP DATABASE regress_log_count;

-- The history horizon: the log stops there, and so does its count.
CREATE DATABASE regress_log_count_horizon;
\c regress_log_count_horizon
\set VERBOSITY terse
CREATE SCHEMA hz VERSIONED;
CREATE TABLE hz.t (id int PRIMARY KEY, v text);
CREATE FUNCTION hz_commits(n int) RETURNS int LANGUAGE plpgsql AS $$
BEGIN
	FOR d IN 1..n LOOP
		INSERT INTO hz.t VALUES (d, 'c' || d);
		PERFORM dolt_commit('-Am', 'c' || d, '--date',
			(timestamptz '2020-01-01 00:00+00' + make_interval(days => d))::text);
	END LOOP;
	RETURN n;
END $$;
SELECT hz_commits(6) AS commits;
SELECT count(*) AS before_severed FROM dolt_log;
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/log-count-hz-' ||
	txid_current() AS hz_url \gset
SELECT dolt_remote('add', 'origin', :'hz_url');
SELECT dolt_push('origin', 'main') AS pushed;
SELECT count(*) > 0 AS severed FROM dolt_history_drop('2020-01-04 00:00+00');
SELECT count(*) AS counted FROM dolt_log;
SELECT count(*) AS rows FROM (SELECT * FROM dolt_log OFFSET 0) l;
-- repaired, the whole history counted again
SELECT dolt_history_repair('origin') > 0 AS repaired;
SELECT count(*) AS repaired_count FROM dolt_log;
SELECT count(*) AS repaired_rows FROM (SELECT * FROM dolt_log OFFSET 0) l;
\c :home
DROP DATABASE regress_log_count_horizon;

-- A root interval: every kept commit's count is exact, from the previous
-- keep and the commits made since, however many the interval skips; a
-- commit between keeps is counted from the keep below it, reading the
-- commits since, not the history.
CREATE DATABASE regress_log_count_interval;
\c regress_log_count_interval
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
CREATE FUNCTION commits_read() RETURNS bigint LANGUAGE sql AS $$
	SELECT coalesce((SELECT value FROM pg_stat_prolly WHERE name = 'commits_read'), 0)
$$;
\set flush 'SELECT pg_stat_force_next_flush() IS NULL AS flushed \\gset'
-- every kept commit's count, against its log's rows
CREATE FUNCTION kept_counts() RETURNS TABLE (kept bigint, counted bigint, right_ones bigint)
LANGUAGE sql AS $$
	SELECT count(*), count(test_commit_commits(k.commit_id)),
		count(*) FILTER (WHERE test_commit_commits(k.commit_id) =
			(SELECT count(*) FROM (SELECT * FROM dolt_log(k.commit_id) OFFSET 0) l))
	FROM test_kept_commits('main') k
$$;
CREATE SCHEMA iv VERSIONED;
CREATE TABLE iv.t (id int PRIMARY KEY, v int);
INSERT INTO iv.t VALUES (1, 0);
CREATE FUNCTION one(i int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	UPDATE iv.t SET v = i WHERE id = 1;
	PERFORM dolt_commit('-am', 'c' || i);
END $$;
-- (each commit its own transaction, as one made by hand is)
CREATE PROCEDURE many(a int, b int) LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN a..b LOOP
		PERFORM one(i);
		COMMIT;
	END LOOP;
END $$;
SET prolly.root_interval_commits = 6;
CALL many(1, 30);
SELECT * FROM kept_counts();
-- the head, between keeps: counted from the keep below
SELECT dolt_reset('--soft', 'HEAD') IS NOT NULL AS still;
SELECT one(31);
:flush
SELECT commits_read() AS before \gset
SELECT count(*) AS head_count FROM dolt_log;
:flush
SELECT commits_read() - :before <= 8 AS read_since_keep;
RESET prolly.root_interval_commits;

-- dolt.transaction_commit: a keep a second at most, the commits between
-- counted into the next
CREATE PROCEDURE updates(n int) LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..n LOOP
		UPDATE iv.t SET v = i WHERE id = 1;
		COMMIT;
	END LOOP;
END $$;
SET dolt.transaction_commit = on;
CALL updates(20);
SELECT pg_sleep(1.1);
UPDATE iv.t SET v = 0 WHERE id = 1;
SET dolt.transaction_commit = off;
SELECT * FROM kept_counts();
SELECT count(*) AS counted, (SELECT count(*) FROM (SELECT * FROM dolt_log OFFSET 0) l) AS rows
FROM dolt_log;
\c :home
DROP DATABASE regress_log_count_interval;
