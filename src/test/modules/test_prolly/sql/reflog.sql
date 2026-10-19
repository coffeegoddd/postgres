-- dolt_reflog: each change of a ref the chunk store still records, newest
-- first, read from the roots it was moved to and the time of each move.
-- Dolt's reflog scripts (DoltReflogTestScripts, but for tags, which prolly
-- hasn't), in a database of their own; commit ids masked.  A database here
-- starts with no commits (Dolt's with "Initialize data repository").
\c postgres
CREATE DATABASE reflog_db;
\c reflog_db
CREATE SCHEMA rl VERSIONED;
SET search_path = rl;
CREATE FUNCTION pg_temp.reflog(VARIADIC args text[] DEFAULT '{}')
RETURNS TABLE (ref text, commit_message text) LANGUAGE sql AS $$
	SELECT ref, commit_message FROM dolt_reflog(VARIADIC args)
$$;

-- Errors.
SELECT * FROM dolt_reflog('foo', 'bar');
SELECT * FROM dolt_reflog('--all', '--all');
SELECT * FROM dolt_reflog(NULL::text);

-- Nothing yet.
SELECT count(*) FROM dolt_reflog();

CREATE TABLE t1 (pk int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'creating table t1')::text) > 0 AS committed;
INSERT INTO t1 VALUES (1);
SELECT length(dolt_commit('-Am', 'inserting row 1')::text) > 0 AS committed;
SELECT dolt_checkout('-b', 'branch1') IS NOT NULL AS checked_out;
INSERT INTO t1 VALUES (2);
SELECT length(dolt_commit('-Am', 'inserting row 2')::text) > 0 AS committed;
INSERT INTO t1 VALUES (3);
SELECT length(dolt_commit('-Am', 'inserting row 3')::text) > 0 AS committed;

-- Every ref's changes, newest first; a change of something else (a
-- working set's checkpoint) isn't one.
SELECT * FROM pg_temp.reflog();
SELECT * FROM pg_temp.reflog('--all');

-- Each entry's commit is the commit its ref was at then, and the times
-- run back.
SELECT bool_and(r.commit_hash = l.commit_hash) AS commits_match
FROM (SELECT commit_hash, row_number() OVER () AS n
	  FROM dolt_reflog('main')) r
JOIN (SELECT commit_hash, row_number() OVER (ORDER BY commit_order DESC) AS n
	  FROM dolt_log('main')) l USING (n);
SELECT bool_and(ref_timestamp IS NOT NULL) AS timed,
	bool_and(ref_timestamp <= now()) AS not_later,
	bool_and(prev IS NULL OR ref_timestamp <= prev) AS newest_first
FROM (SELECT ref_timestamp, lag(ref_timestamp) OVER () AS prev
	  FROM dolt_reflog()) r;

-- One ref, by its full name or its name alone, case aside.
SELECT * FROM pg_temp.reflog('doesNotExist');
SELECT * FROM pg_temp.reflog('refs/heads/main');
SELECT * FROM pg_temp.reflog('reFS/Heads/MaIn');
SELECT * FROM pg_temp.reflog('main');
SELECT * FROM pg_temp.reflog('MaIN');
SELECT * FROM pg_temp.reflog('refs/heads/branch1');
SELECT * FROM pg_temp.reflog('branch1', '--all');

-- A branch deleted is still in the reflog.
SELECT dolt_checkout('main') IS NOT NULL AS checked_out;
SELECT dolt_branch('-D', 'branch1') IS NOT NULL AS deleted;
SELECT * FROM pg_temp.reflog('branch1');

-- A head moved back is a change too.
SELECT dolt_reset('--hard', 'HEAD~1') IS NOT NULL AS reset;
SELECT * FROM pg_temp.reflog('main');

-- A garbage collection forgets the moves before it.
SELECT dolt_gc() IS NOT NULL AS collected;
\c reflog_db
SELECT count(*) FROM dolt_reflog();
SELECT dolt_checkout('-b', 'after_gc') IS NOT NULL AS checked_out;
INSERT INTO rl.t1 VALUES (4);
SELECT length(dolt_commit('-Am', 'after the collection')::text) > 0 AS committed;
SELECT ref, commit_message FROM dolt_reflog();

\c postgres
DROP DATABASE reflog_db;
