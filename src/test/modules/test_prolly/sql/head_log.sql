-- Head logs: each branch's head moves in order, each with when it was made
-- and the commit it moved to, so the commit a branch was at at a time is a
-- seek.  A move is logged with its transaction: a commit, a merge, a reset,
-- a transaction's own commit (dolt.transaction_commit), a pull's
-- fast-forward; a remote-tracking branch's as a fetch moves it.

\getenv abs_builddir PG_ABS_BUILDDIR
\set dir :abs_builddir '/tmp_check/head_log'
SELECT 'file://' || :'dir' || '/' || txid_current() AS url \gset
SELECT current_database() AS home \gset
-- (a database of its own: its history is only this test's)
CREATE DATABASE regress_head_log;
\c regress_head_log

CREATE SCHEMA hl VERSIONED;
CREATE TABLE hl.t (id int PRIMARY KEY, v int);

-- logged(ref): does ref's log end at its head, in order of number and
-- date, each number once?
CREATE FUNCTION hl_logged(ref text DEFAULT 'main') RETURNS text
LANGUAGE sql AS $$
	WITH l AS (SELECT seq, moved_at, commit_hash,
					  lag(moved_at) OVER (ORDER BY seq) AS before,
					  row_number() OVER (ORDER BY seq) - 1 AS n
			   FROM prolly_head_log(ref))
	SELECT format('%s moves, numbered %s, dated %s, ends at %s',
				  count(*),
				  CASE WHEN bool_and(seq = n) THEN 'in order' ELSE 'out of order' END,
				  CASE WHEN bool_and(before IS NULL OR before <= moved_at)
					   THEN 'rising' ELSE 'out of order' END,
				  CASE WHEN (SELECT commit_hash FROM l ORDER BY seq DESC LIMIT 1) =
							dolt_hashof(ref) THEN 'the head' ELSE 'another' END)
	FROM l
$$;

SELECT length(dolt_commit('-Am', 'hl 1')::text) AS committed;
SELECT hl_logged();
INSERT INTO hl.t VALUES (1, 1);
SELECT length(dolt_commit('-am', 'hl 2')::text) AS committed;
SELECT now() AS between \gset
SELECT pg_sleep(0.01);
INSERT INTO hl.t VALUES (2, 2);
SELECT length(dolt_commit('-am', 'hl 3')::text) AS committed;
SELECT hl_logged();
-- The commit the head was at at a time: one move's; none before the first.
SELECT prolly_head_at('main', :'between') = dolt_hashof('HEAD~1') AS at_between,
	   prolly_head_at('main', now()) = dolt_hashof('HEAD') AS at_now,
	   prolly_head_at('main', '2000-01-01') IS NULL AS before_any;
SELECT count(*) AS from_between,
	   (SELECT count(*) FROM prolly_head_log('main', :'between', NULL, true)) AS backward
FROM prolly_head_log('main', :'between', NULL, false);

-- A reset is a move, back to a commit made before.
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT hl_logged();
SELECT commit_hash = dolt_hashof('HEAD') AS reset_logged
FROM prolly_head_log('main') ORDER BY seq DESC LIMIT 1;

-- A commit dated otherwise (--date) is logged when it was made.
INSERT INTO hl.t VALUES (3, 3);
SELECT length(dolt_commit('-am', 'hl dated', '--date', '2001-02-03')::text) AS committed;
SELECT moved_at > '2020-01-01' AS logged_now FROM prolly_head_log('main')
ORDER BY seq DESC LIMIT 1;
SELECT prolly_head_at('main', now()) = dolt_hashof('HEAD') AS at_now;

-- What rolls back logs nothing.
SELECT count(*) AS moves FROM prolly_head_log('main') \gset
BEGIN;
INSERT INTO hl.t VALUES (10, 10);
SELECT length(dolt_commit('-am', 'hl undone')::text) AS committed;
ROLLBACK;
BEGIN;
SAVEPOINT s;
INSERT INTO hl.t VALUES (11, 11);
SELECT length(dolt_commit('-am', 'hl sub')::text) AS committed;
ROLLBACK TO s;
COMMIT;
SELECT count(*) - :moves AS new_moves FROM prolly_head_log('main');
SELECT hl_logged();

-- dolt.transaction_commit: each transaction's commit is a move.  Forty of
-- them: the latest moves wait beside the head, then go into the log's
-- tree, in order, together.
SET dolt.transaction_commit = on;
DO $$
BEGIN
	FOR i IN 1..40 LOOP
		INSERT INTO hl.t VALUES (100 + i, i);
		COMMIT;
	END LOOP;
END $$;
BEGIN;
INSERT INTO hl.t VALUES (99, 99);
ROLLBACK;
RESET dolt.transaction_commit;
SELECT count(*) - :moves AS txn_moves FROM prolly_head_log('main');
SELECT hl_logged();
-- every move logged is a commit of the head's history, newest last
SELECT count(*) AS not_in_history FROM prolly_head_log('main') l
WHERE l.seq > :moves AND NOT EXISTS
	(SELECT 1 FROM dolt_log('main') d WHERE d.commit_hash = l.commit_hash);
SELECT (SELECT count(*) FROM prolly_head_log('main', NULL, now(), true)) =
	   (SELECT count(*) FROM prolly_head_log('main')) AS all_before_now;

-- A rename keeps the log; a delete lets it go.
SELECT dolt_branch('side');
SELECT dolt_checkout('side');
INSERT INTO hl.t VALUES (200, 200);
SELECT length(dolt_commit('-am', 'hl side')::text) AS committed;
SELECT dolt_checkout('main');
SELECT count(*) AS side_moves FROM prolly_head_log('side');
SELECT dolt_branch('-m', 'side', 'side2');
SELECT count(*) AS renamed_moves, (SELECT count(*) FROM prolly_head_log('side')) AS old_name
FROM prolly_head_log('side2');
SELECT dolt_branch('-D', 'side2');
SELECT dolt_branch('side2');
SELECT count(*) AS remade_moves FROM prolly_head_log('side2');
SELECT dolt_branch('-D', 'side2');

-- A remote-tracking branch's log, as a push and a fetch move it.
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_push('origin', 'main') AS pushed;
SELECT dolt_fetch('origin') AS fetched;
SELECT hl_logged('remotes/origin/main');

-- A clone's heads, and a pull fast-forwarded to commits made elsewhere:
-- one move each.
CREATE DATABASE regress_head_log_clone;
\c regress_head_log_clone
SELECT dolt_clone(:'url');
SELECT count(*) AS clone_moves, bool_and(commit_hash = dolt_hashof('main')) AS at_head
FROM prolly_head_log('main');
INSERT INTO hl.t VALUES (300, 300);
SELECT length(dolt_commit('-am', 'hl elsewhere 1')::text) AS committed;
INSERT INTO hl.t VALUES (301, 301);
SELECT length(dolt_commit('-am', 'hl elsewhere 2')::text) AS committed;
SELECT dolt_push('origin', 'main') AS pushed;
\c regress_head_log
DROP DATABASE regress_head_log_clone;
SELECT count(*) AS moves FROM prolly_head_log('main') \gset
SELECT (dolt_pull('origin', 'main'))[2] AS fast_forward;
SELECT count(*) - :moves AS pulled_moves FROM prolly_head_log('main');
SELECT hl_logged();

\c :home
DROP DATABASE regress_head_log;
