-- What a commit writes to the refs is bounded, whatever the history: every
-- branch has a history window, past which its commits' roots are let go of
-- with their entries (the kept commit, its counts, its index entries), so
-- the refs a branch holds are bounded by its window.  A one-row commit
-- after 50 commits, and after 200, writes a few refs nodes and bytes, the
-- same both times.
SELECT current_database() AS home \gset
CREATE DATABASE refs_per_commit;
-- (a window that holds the newest commit only)
ALTER DATABASE refs_per_commit SET prolly.history_window = '1 microsecond';
\c refs_per_commit
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
\set flush 'SELECT pg_stat_force_next_flush() IS NULL AS flushed \\gset'
CREATE FUNCTION one_commit(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	UPDATE rc.t SET v = v + 1 WHERE id = n % 100 + 1;
	PERFORM dolt_commit('-am', 'c' || n);
END $$;
CREATE TEMP TABLE measured (at int, refs_nodes bigint, bytes bigint, live bigint);
CREATE FUNCTION measure(at int) RETURNS void LANGUAGE sql AS $$
	INSERT INTO measured SELECT at,
		(SELECT value FROM pg_stat_prolly WHERE name = 'refs_nodes_written'),
		(SELECT value FROM pg_stat_prolly WHERE name = 'chunk_bytes_appended'),
		(SELECT count(*) FROM test_kept_commits('main'))
$$;

CREATE SCHEMA rc VERSIONED;
CREATE TABLE rc.t (id int PRIMARY KEY, v int);
INSERT INTO rc.t SELECT g, 0 FROM generate_series(1, 100) g;
SELECT dolt_commit('-Am', 'c0') IS NOT NULL AS committed;
-- (each its own transaction, as a commit made by hand is)
SELECT format('SELECT one_commit(%s)', i) FROM generate_series(1, 49) i \gexec
:flush
SELECT pg_stat_reset_shared('prolly');
SELECT one_commit(50);
:flush
SELECT measure(50);
SELECT format('SELECT one_commit(%s)', i) FROM generate_series(51, 199) i \gexec
:flush
SELECT pg_stat_reset_shared('prolly');
SELECT one_commit(200);
:flush
SELECT measure(200);

-- a few refs nodes, a few pages of bytes, a root or two kept: and no more
-- after 200 commits than after 50
SELECT at, refs_nodes <= 10 AS few_refs_nodes, bytes <= 25000 AS few_bytes,
	live <= 2 AS bounded_roots
FROM measured ORDER BY at;
SELECT (SELECT refs_nodes FROM measured WHERE at = 200) <=
	(SELECT refs_nodes FROM measured WHERE at = 50) + 1 AS refs_nodes_flat,
	(SELECT bytes FROM measured WHERE at = 200) <=
	(SELECT bytes FROM measured WHERE at = 50) * 3 / 2 AS bytes_flat;

\c :home
DROP DATABASE refs_per_commit;
