-- Each commit records the commits it reaches, itself included (one more
-- than its first parent's, and for a merge, more by those its other
-- parents reach that the first doesn't), as Dolt's commit closure counts
-- them.  So count(*) of dolt_log(ref) alone is planned as a scan giving
-- the count ref's commit records, reading one commit, not the history; a
-- history horizon, or a commit recording none, has the log counted as it
-- gives its rows.  A remote's commit whose count isn't its parents' is
-- refused.
SELECT current_database() AS home \gset
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

-- A commit recording no count (made elsewhere, or before counts were):
-- the log counted; and the commits made on it record none.
SELECT test_commit_chain(dolt_hashof('main'), 3) AS old_head \gset
SELECT test_commit_commits(:'old_head') AS recorded_on_old;
SELECT counts(:'old_head') AS counted;
SELECT dolt_reset('--hard', :'old_head') IS NOT NULL AS reset;
UPDATE lc.t SET v = 0 WHERE id = 1;
SELECT length(dolt_commit('-Am', 'on old')::text) AS committed;
SELECT test_commit_commits(dolt_hashof('main')) AS recorded_on_new,
	counts('main') AS counted;

-- A remote's commit recording a count its parent doesn't give is
-- refused: a clone receives nothing.
SELECT test_commit_with_commits(dolt_hashof('a'), 2) AS forged \gset
SELECT test_commit_commits(:'forged') AS forged_count;
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/log-count-' ||
	txid_current() AS url \gset
SELECT dolt_branch('forged', :'forged');
SELECT dolt_remote('add', 'fr', :'url');
SELECT dolt_push('fr', 'forged') AS pushed;
SELECT test_commit_with_commits(dolt_hashof('a'), test_commit_commits(dolt_hashof('a')) + 1) AS right \gset
SELECT dolt_branch('right', :'right');
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/log-count-right-' ||
	txid_current() AS right_url \gset
SELECT dolt_remote('add', 'rr', :'right_url');
SELECT dolt_push('rr', 'right') AS pushed;
SELECT counts('right') AS right_counts;
CREATE DATABASE regress_log_count_received;
\c regress_log_count_received
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE FUNCTION pg_temp.err(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN regexp_replace(SQLERRM, '[0-9a-f]{32,}', '<hash>', 'g');
END $$;
SELECT pg_temp.err(format('SELECT dolt_clone(%L)', :'url')) AS cloned;
SELECT count(*) AS branches FROM dolt_branches;
\c :home
DROP DATABASE regress_log_count_received;
-- (the right count is taken, the clone's count the same)
CREATE DATABASE regress_log_count_right;
\c regress_log_count_right
SELECT dolt_clone(:'right_url') IS NOT NULL AS cloned;
SELECT (SELECT count(*) FROM dolt_log('right')) || ' ' ||
	(SELECT count(*) FROM (SELECT * FROM dolt_log('right') OFFSET 0) l) AS cloned_counts;
\c :home
DROP DATABASE regress_log_count_right;
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
SELECT count(*) > 0 AS severed FROM dolt_history_drop('2020-01-04 00:00+00');
SELECT count(*) AS counted FROM dolt_log;
SELECT count(*) AS rows FROM (SELECT * FROM dolt_log OFFSET 0) l;
\c :home
DROP DATABASE regress_log_count_horizon;
