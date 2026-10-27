-- Moves to commits by their captured roots: a hard reset to any kept
-- commit, however far back, gives each table its pages in O(1); one to a
-- commit between roots restores the nearest before it and writes the rows
-- between; a branch made at a commit moves back by the roots another
-- branch kept; and roots are captured no more often than the interval asks,
-- at a transaction's own commit too.

SELECT current_database() AS home \gset
CREATE DATABASE root_moves;
ALTER DATABASE root_moves SET prolly.restore_min_changes = 0;
\c root_moves
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;

CREATE FUNCTION counted() RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(name, value) FROM pg_stat_prolly
	WHERE name IN ('apply_rows', 'relations_restored', 'move_tables_shared', 'pages_kept')
$$;
CREATE FUNCTION since(before jsonb) RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$
	SELECT key, value::bigint - (before ->> key)::bigint
	FROM jsonb_each_text(counted()) ORDER BY 1
$$;

CREATE SCHEMA rm VERSIONED;
CREATE TABLE rm.t (id int PRIMARY KEY, v int NOT NULL, w text NOT NULL);
CREATE INDEX t_v ON rm.t (v);
INSERT INTO rm.t SELECT g, 0, repeat('w', 100) FROM generate_series(1, 5000) g;
CREATE FUNCTION digest() RETURNS text LANGUAGE sql AS $$
	SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM rm.t t
$$;
SELECT dolt_commit('-Am', 'c0') IS NOT NULL AS committed;
CREATE TEMP TABLE digests (n int, d text);
INSERT INTO digests VALUES (0, digest());
DO $$
BEGIN
	FOR i IN 1..30 LOOP
		UPDATE rm.t SET v = i WHERE id % 50 = i;
		PERFORM dolt_commit('-am', 'c' || i);
		INSERT INTO digests VALUES (i, digest());
	END LOOP;
END $$;
SELECT count(*) AS kept FROM test_kept_commits('main');

-- far back, with a dirty working set: every relation given its pages, no
-- row written
SELECT dolt_hashof('HEAD') AS c30 \gset
UPDATE rm.t SET v = -1 WHERE id <= 2000;
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', dolt_hashof('HEAD~25'));
SELECT pg_stat_force_next_flush();
SELECT name, CASE WHEN name = 'apply_rows' THEN value ELSE (value > 0)::int END
FROM since(:'before') WHERE name <> 'pages_kept';
SELECT digest() = (SELECT d FROM digests WHERE n = 5) AS at_c5,
	NOT EXISTS (SELECT FROM dolt_status) AS clean;
SET enable_seqscan = off;
SELECT count(*) AS by_index FROM rm.t WHERE v = 5;
RESET enable_seqscan;

-- and forward again to the newest, which stays kept
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'c30');
SELECT pg_stat_force_next_flush();
SELECT name, CASE WHEN name = 'apply_rows' THEN value ELSE (value > 0)::int END
FROM since(:'before') WHERE name <> 'pages_kept';
SELECT digest() = (SELECT d FROM digests WHERE n = 30) AS at_c30;

-- a branch made at the head, used and changed, moves back by the roots
-- main kept
SELECT held AS roots_before FROM test_bmap_roots() \gset
SELECT dolt_branch('scratch', :'c30');
SELECT dolt_checkout('scratch');
UPDATE rm.t SET v = -2 WHERE id <= 3000;
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', (SELECT commit_hash FROM dolt_log WHERE message = 'c12'));
SELECT pg_stat_force_next_flush();
-- (the pages held by the branch's own roots too: its keep after the move
-- finds them unchanged, and captures none)
SELECT name, CASE WHEN name = 'pages_kept' THEN value ELSE (value > 0)::int END
FROM since(:'before');
SELECT digest() = (SELECT d FROM digests WHERE n = 12) AS at_c12;
SELECT count(*) AS scratch_kept FROM test_kept_commits('scratch');
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'scratch');
-- (deleted, the branch's roots are let go of: the store keeps no more than
-- before it)
SELECT held = :roots_before AS scratch_roots_let_go FROM test_bmap_roots();

-- roots an interval of commits apart: a move between them restores the
-- nearest before and writes the rows between, no more
SET prolly.root_interval_commits = 4;
DO $$
BEGIN
	FOR i IN 31..42 LOOP
		UPDATE rm.t SET v = i WHERE id % 50 = i - 30;
		PERFORM dolt_commit('-am', 'c' || i);
		INSERT INTO digests VALUES (i, digest());
	END LOOP;
END $$;
SELECT count(*) AS kept FROM test_kept_commits('main');
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', (SELECT commit_hash FROM dolt_log WHERE message = 'c39'));
SELECT pg_stat_force_next_flush();
SELECT name, CASE WHEN name = 'apply_rows' THEN (value BETWEEN 1 AND 4 * 100)::int
	ELSE (value > 0)::int END FROM since(:'before') WHERE name <> 'pages_kept';
SELECT digest() = (SELECT d FROM digests WHERE n = 39) AS at_c39;
RESET prolly.root_interval_commits;

-- a branch made at the head, with a kept commit of its own, moves to a
-- commit between main's roots as main does: by the nearest root main kept
-- below the branch's own
SELECT dolt_hashof('HEAD') AS at_c39_hash \gset
SELECT dolt_branch('scratch2', :'at_c39_hash');
SELECT dolt_checkout('scratch2');
UPDATE rm.t SET v = -6 WHERE id = 1;
SELECT dolt_commit('-am', 's1') IS NOT NULL AS committed;
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', (SELECT commit_hash FROM dolt_log WHERE message = 'c33'));
SELECT pg_stat_force_next_flush();
SELECT name, CASE WHEN name = 'apply_rows' THEN (value BETWEEN 1 AND 4 * 100)::int
	ELSE (value > 0)::int END FROM since(:'before') WHERE name <> 'pages_kept';
SELECT digest() = (SELECT d FROM digests WHERE n = 33) AS at_c33;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'scratch2');

-- a transaction's own commit captures a root when one is due: every
-- commit with no interval, none within a long one
SET dolt.transaction_commit = on;
SET prolly.root_interval = 0;
SELECT count(*) AS kept_before FROM test_kept_commits('main') \gset
UPDATE rm.t SET v = 100 WHERE id = 1;
UPDATE rm.t SET v = 101 WHERE id = 1;
SELECT count(*) - :kept_before AS kept_each FROM test_kept_commits('main');
SET prolly.root_interval = '1h';
SELECT count(*) AS kept_before FROM test_kept_commits('main') \gset
UPDATE rm.t SET v = 102 WHERE id = 1;
UPDATE rm.t SET v = 103 WHERE id = 1;
SELECT count(*) - :kept_before AS kept_within FROM test_kept_commits('main');
RESET prolly.root_interval;
RESET dolt.transaction_commit;

-- a merge between the nearest root and the commit, with a dirty working
-- set: the nearest root is restored and the move ends at the merge's
-- rows, its other parent's included, writing no more rows than the
-- commits between changed (a root on the other parent's side is found by
-- the walk only when on the first parents)
SET prolly.root_interval_commits = 100;
SELECT dolt_branch('side');
SELECT dolt_checkout('side');
UPDATE rm.t SET v = 200 WHERE id % 50 = 45;
SELECT dolt_commit('-am', 'side1') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
UPDATE rm.t SET v = 201 WHERE id % 50 = 46;
SELECT dolt_commit('-am', 'm1') IS NOT NULL AS committed;
SELECT dolt_merge('side') IS NOT NULL AS merged;
SELECT dolt_hashof('HEAD') AS merged_at \gset
INSERT INTO digests VALUES (100, digest());
UPDATE rm.t SET v = 202 WHERE id % 50 BETWEEN 10 AND 30;
SELECT dolt_commit('-am', 'm2') IS NOT NULL AS committed;
UPDATE rm.t SET v = -3 WHERE id <= 3000;
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'merged_at');
SELECT pg_stat_force_next_flush();
SELECT name, CASE WHEN name = 'apply_rows' THEN (value <= 3 * 100)::int
	ELSE value END FROM since(:'before') WHERE name <> 'pages_kept';
SELECT digest() = (SELECT d FROM digests WHERE n = 100) AS at_merge,
	NOT EXISTS (SELECT FROM dolt_status) AS clean;
SELECT dolt_branch('-D', 'side');
RESET prolly.root_interval_commits;

-- a move to a commit older than any the branch keeps looks no further
-- back than the oldest it keeps: a few commits read, not the history
ALTER DATABASE root_moves SET prolly.history_min_commits = 2;
DO $$
BEGIN
	FOR i IN 1..20 LOOP
		UPDATE rm.t SET v = 300 + i WHERE id % 50 = i;
		PERFORM dolt_commit('-am', 'k' || i);
		INSERT INTO digests VALUES (300 + i, digest());
	END LOOP;
END $$;
SELECT test_retain_roots('main');
SELECT count(*) AS kept FROM test_kept_commits('main');
CREATE FUNCTION reads() RETURNS bigint LANGUAGE sql AS $$
	SELECT sum(value)::bigint FROM pg_stat_prolly
	WHERE name IN ('commits_read', 'commit_cache_hits')
$$;
SELECT pg_stat_force_next_flush();
SELECT reads() AS reads_before \gset
SELECT dolt_reset('--hard', dolt_hashof('HEAD~10'));
SELECT pg_stat_force_next_flush();
SELECT reads() - :reads_before < 40 AS few_commits_read;
SELECT digest() = (SELECT d FROM digests WHERE n = 310) AS at_k10;
ALTER DATABASE root_moves RESET prolly.history_min_commits;

\c :home
DROP DATABASE root_moves;
