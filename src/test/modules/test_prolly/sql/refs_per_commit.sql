-- What a commit writes to the refs is bounded, whatever the history and
-- whatever the history window: a branch's kept commits (their entries,
-- root counts and data-tree index) are one region of the refs, after
-- every other entry, so a commit's own land at that region's right end;
-- the branch's few small entries (head, mover, baseline, summary) share
-- leaves; and the transaction writes its refs once.  A one-row commit
-- after 50 commits and after 200 (the mean of five, each its own
-- transaction), under a window that holds the newest commit only, under
-- the default month and under a century, writes a few refs nodes each
-- time, the same few.
SELECT current_database() AS home \gset
CREATE TABLE refs_measured (win text, at int, refs_nodes bigint, bytes bigint,
	live bigint);

-- window: 1 microsecond
CREATE DATABASE refs_per_commit;
ALTER DATABASE refs_per_commit SET prolly.history_window = '1 microsecond';
\c refs_per_commit
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
CREATE FUNCTION one_commit(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	UPDATE rc.t SET v = v + 1 WHERE id = n % 100 + 1;
	PERFORM dolt_commit('-am', 'c' || n);
END $$;
-- (each commit its own transaction, as one made by hand is)
CREATE PROCEDURE commits(a int, b int) LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN a..b LOOP
		PERFORM one_commit(i);
		COMMIT;
	END LOOP;
END $$;
CREATE FUNCTION measured(at int) RETURNS text LANGUAGE sql AS $$
	SELECT format('(%L, %s, %s, %s, %s)', '1 microsecond', at,
		(SELECT value / 5 FROM pg_stat_prolly WHERE name = 'refs_nodes_written'),
		(SELECT value / 5 FROM pg_stat_prolly WHERE name = 'chunk_bytes_appended'),
		(SELECT count(*) FROM test_kept_commits('main')))
$$;
CREATE SCHEMA rc VERSIONED;
CREATE TABLE rc.t (id int PRIMARY KEY, v int);
INSERT INTO rc.t SELECT g, 0 FROM generate_series(1, 100) g;
SELECT dolt_commit('-Am', 'c0') IS NOT NULL AS committed;
CALL commits(1, 45);
SELECT pg_stat_force_next_flush() IS NULL AS flushed \gset
SELECT pg_stat_reset_shared('prolly');
CALL commits(46, 50);
SELECT pg_stat_force_next_flush() IS NULL AS flushed \gset
SELECT measured(50) AS m50 \gset
CALL commits(51, 195);
SELECT pg_stat_force_next_flush() IS NULL AS flushed \gset
SELECT pg_stat_reset_shared('prolly');
CALL commits(196, 200);
SELECT pg_stat_force_next_flush() IS NULL AS flushed \gset
SELECT measured(200) AS m200 \gset
\c :home
INSERT INTO refs_measured VALUES :m50, :m200;
DROP DATABASE refs_per_commit;

-- window: a month (the default)
CREATE DATABASE refs_per_commit;
\c refs_per_commit
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
CREATE FUNCTION one_commit(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	UPDATE rc.t SET v = v + 1 WHERE id = n % 100 + 1;
	PERFORM dolt_commit('-am', 'c' || n);
END $$;
-- (each commit its own transaction, as one made by hand is)
CREATE PROCEDURE commits(a int, b int) LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN a..b LOOP
		PERFORM one_commit(i);
		COMMIT;
	END LOOP;
END $$;
CREATE FUNCTION measured(at int) RETURNS text LANGUAGE sql AS $$
	SELECT format('(%L, %s, %s, %s, %s)', 'a month (the default)', at,
		(SELECT value / 5 FROM pg_stat_prolly WHERE name = 'refs_nodes_written'),
		(SELECT value / 5 FROM pg_stat_prolly WHERE name = 'chunk_bytes_appended'),
		(SELECT count(*) FROM test_kept_commits('main')))
$$;
CREATE SCHEMA rc VERSIONED;
CREATE TABLE rc.t (id int PRIMARY KEY, v int);
INSERT INTO rc.t SELECT g, 0 FROM generate_series(1, 100) g;
SELECT dolt_commit('-Am', 'c0') IS NOT NULL AS committed;
CALL commits(1, 45);
SELECT pg_stat_force_next_flush() IS NULL AS flushed \gset
SELECT pg_stat_reset_shared('prolly');
CALL commits(46, 50);
SELECT pg_stat_force_next_flush() IS NULL AS flushed \gset
SELECT measured(50) AS m50 \gset
CALL commits(51, 195);
SELECT pg_stat_force_next_flush() IS NULL AS flushed \gset
SELECT pg_stat_reset_shared('prolly');
CALL commits(196, 200);
SELECT pg_stat_force_next_flush() IS NULL AS flushed \gset
SELECT measured(200) AS m200 \gset
\c :home
INSERT INTO refs_measured VALUES :m50, :m200;
DROP DATABASE refs_per_commit;

-- window: 100 years
CREATE DATABASE refs_per_commit;
ALTER DATABASE refs_per_commit SET prolly.history_window = '100 years';
\c refs_per_commit
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
CREATE FUNCTION one_commit(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	UPDATE rc.t SET v = v + 1 WHERE id = n % 100 + 1;
	PERFORM dolt_commit('-am', 'c' || n);
END $$;
-- (each commit its own transaction, as one made by hand is)
CREATE PROCEDURE commits(a int, b int) LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN a..b LOOP
		PERFORM one_commit(i);
		COMMIT;
	END LOOP;
END $$;
CREATE FUNCTION measured(at int) RETURNS text LANGUAGE sql AS $$
	SELECT format('(%L, %s, %s, %s, %s)', '100 years', at,
		(SELECT value / 5 FROM pg_stat_prolly WHERE name = 'refs_nodes_written'),
		(SELECT value / 5 FROM pg_stat_prolly WHERE name = 'chunk_bytes_appended'),
		(SELECT count(*) FROM test_kept_commits('main')))
$$;
CREATE SCHEMA rc VERSIONED;
CREATE TABLE rc.t (id int PRIMARY KEY, v int);
INSERT INTO rc.t SELECT g, 0 FROM generate_series(1, 100) g;
SELECT dolt_commit('-Am', 'c0') IS NOT NULL AS committed;
CALL commits(1, 45);
SELECT pg_stat_force_next_flush() IS NULL AS flushed \gset
SELECT pg_stat_reset_shared('prolly');
CALL commits(46, 50);
SELECT pg_stat_force_next_flush() IS NULL AS flushed \gset
SELECT measured(50) AS m50 \gset
CALL commits(51, 195);
SELECT pg_stat_force_next_flush() IS NULL AS flushed \gset
SELECT pg_stat_reset_shared('prolly');
CALL commits(196, 200);
SELECT pg_stat_force_next_flush() IS NULL AS flushed \gset
SELECT measured(200) AS m200 \gset
\c :home
INSERT INTO refs_measured VALUES :m50, :m200;
DROP DATABASE refs_per_commit;

-- every window, after 50 commits and after 200: a few refs nodes (4 under
-- the tiny window, 8 or 9 under the others, where the refs tree holds the
-- window's entries: the region's right end, the small entries' leaf, the
-- data-tree index's, and the levels above), a few nodes' bytes (nodes are
-- about 4 KB once the tree is full), and no more nodes after 200 than
-- after 50
SELECT win, at, refs_nodes <= 10 AS few_refs_nodes, bytes <= 36000 AS few_bytes
FROM refs_measured ORDER BY win, at;
SELECT a.win, b.refs_nodes <= a.refs_nodes + 2 AS refs_nodes_flat
FROM refs_measured a JOIN refs_measured b ON a.win = b.win AND a.at = 50 AND b.at = 200
ORDER BY a.win;
-- the window bounds what is kept
SELECT win, live FROM refs_measured WHERE at = 200 ORDER BY win;
DROP TABLE refs_measured;
