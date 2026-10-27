-- Moves to commits by their captured roots: a hard reset to any kept
-- commit, however far back, gives each table its pages in O(1); one to a
-- commit between roots restores the nearest before it and writes the rows
-- between; a branch made at a commit moves back by the roots another
-- branch kept; and roots are captured no more often than the interval asks,
-- at a transaction's own commit too.

SELECT current_database() AS home \gset
CREATE DATABASE root_moves;
ALTER DATABASE root_moves SET prolly.kept_commits = 1000;
ALTER DATABASE root_moves SET prolly.restore_min_changes = 0;
\c root_moves
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;

CREATE FUNCTION counted() RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(name, value) FROM pg_stat_prolly
	WHERE name IN ('apply_rows', 'relations_restored', 'pages_kept')
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
SELECT dolt_branch('scratch', :'c30');
SELECT dolt_checkout('scratch');
UPDATE rm.t SET v = -2 WHERE id <= 3000;
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', (SELECT commit_hash FROM dolt_log WHERE message = 'c12'));
SELECT pg_stat_force_next_flush();
SELECT name, CASE WHEN name = 'apply_rows' THEN value ELSE (value > 0)::int END
FROM since(:'before') WHERE name <> 'pages_kept';
SELECT digest() = (SELECT d FROM digests WHERE n = 12) AS at_c12;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'scratch');

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

\c :home
DROP DATABASE root_moves;
