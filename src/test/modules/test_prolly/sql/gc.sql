-- Garbage collection of the chunk store: what the root reaches (and what
-- was appended since the seal) is copied to a new, settled table file;
-- the rest is gone once the swap's readers have ended.  A default
-- collection stops at settled files.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE DATABASE gc_db;
\c gc_db
CREATE EXTENSION test_prolly;

-- A root reaching a node and its child; another chunk nothing reaches.
SELECT test_chunk_put('gc a') AS a \gset
SELECT test_chunk_put('gc b') AS b \gset
SELECT test_chunk_put_node(1, ARRAY[:'a']) AS node \gset
SELECT test_chunk_move_root(NULL, :'node');
SELECT kept, freed FROM test_chunk_gc(false);
SELECT test_chunk_has(:'a') AS a, test_chunk_has(:'node') AS node,
	test_chunk_has(:'b') AS b;
SELECT test_chunk_root() = :'node' AS root;
SELECT name, chunks, settled FROM test_chunk_table_files();
SELECT name, chunks FROM test_chunk_segments();

-- Another session reads the new files.
\c gc_db
SELECT convert_from(test_chunk_get(:'a'), 'UTF8') AS a, test_chunk_has(:'b') AS b;

-- Nothing new: nothing collected, the settled file kept as it is.
SELECT kept, freed FROM test_chunk_gc(false);
SELECT name, chunks, settled FROM test_chunk_table_files();

-- What changed since: only it is collected; the settled file is kept.
SELECT test_chunk_put('gc c') AS c \gset
SELECT test_chunk_put('gc d') AS d \gset
SELECT test_chunk_put_node(2, ARRAY[:'node', :'c']) AS node2 \gset
SELECT test_chunk_move_root(:'node', :'node2');
SELECT kept, freed FROM test_chunk_gc(false);
SELECT test_chunk_has(:'c') AS c, test_chunk_has(:'d') AS d,
	test_chunk_has(:'a') AS a, test_chunk_has(:'node2') AS node2;
SELECT name, chunks, settled FROM test_chunk_table_files();

-- Table files a transfer added, not settled, are collected.
SELECT test_chunk_table_file(ARRAY['gc tf live'::bytea, 'gc tf dead'::bytea]);
SELECT test_chunk_put_node(1, ARRAY[:'node2', test_content_hash('gc tf live')]) AS node3 \gset
SELECT test_chunk_move_root(:'node2', :'node3');
SELECT kept, freed FROM test_chunk_gc(false);
SELECT test_chunk_has(test_content_hash('gc tf live')) AS live,
	test_chunk_has(test_content_hash('gc tf dead')) AS dead;
SELECT name, chunks, settled FROM test_chunk_table_files();

-- A collection's files survive a new session and a conjoin.
SELECT test_chunk_conjoin();
SELECT name, chunks, settled FROM test_chunk_table_files();
\c gc_db
SELECT test_chunk_has(:'a') AS a, test_chunk_has(:'c') AS c,
	test_chunk_root() = :'node3' AS root;

-- Branches and commits: a branch made and deleted since the last
-- collection has its commits collected; the default branch's history
-- isn't.  One collected while it lived is settled: a default collection
-- keeps it.
\c postgres
CREATE DATABASE gc_dolt;
\c gc_dolt
CREATE EXTENSION test_prolly;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
INSERT INTO app.t SELECT g, repeat('x', 100) || g FROM generate_series(1, 200) g;
SELECT length(dolt_commit('-m', 'first')::text) AS committed;
SELECT dolt_branch('kept');
SELECT dolt_checkout('kept');
INSERT INTO app.t SELECT g, repeat('k', 100) || g FROM generate_series(201, 300) g;
SELECT length(dolt_commit('-m', 'on kept')::text) AS committed;
SELECT dolt_hashof('kept') AS kept_head \gset
SELECT dolt_checkout('main');
SELECT dolt_gc();
SELECT count(*) AS commits FROM dolt_log('kept');
SELECT dolt_branch('side');
SELECT dolt_checkout('side');
INSERT INTO app.t SELECT g, repeat('y', 100) || g FROM generate_series(201, 400) g;
SELECT length(dolt_commit('-m', 'on side')::text) AS committed;
SELECT dolt_hashof('side') AS side_head \gset
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'side', 'kept');
SELECT pg_stat_force_next_flush();
SELECT (SELECT value FROM pg_stat_prolly WHERE name = 'gc_bytes_copied') AS gc_before,
	(SELECT value FROM pg_stat_prolly WHERE name = 'table_file_bytes_written') AS tf_before,
	(SELECT value FROM pg_stat_prolly WHERE name = 'manifest_bytes_written') AS mf_before \gset
SELECT kept > 0 AS kept, freed > 0 AS freed FROM test_chunk_gc(false);
-- What it kept it copied, into table files; its manifest and list of
-- dropped chunks are the store's metadata.
SELECT pg_stat_force_next_flush();
SELECT (SELECT value FROM pg_stat_prolly WHERE name = 'gc_bytes_copied') > :gc_before AS copied,
	(SELECT value FROM pg_stat_prolly WHERE name = 'table_file_bytes_written') - :tf_before >=
	(SELECT value FROM pg_stat_prolly WHERE name = 'gc_bytes_copied') - :gc_before
		AS copies_in_table_files,
	(SELECT value FROM pg_stat_prolly WHERE name = 'manifest_bytes_written') > :mf_before
		AS metadata_written;
SELECT test_chunk_has(:'side_head') AS side_head,
	test_chunk_has(:'kept_head') AS kept_head;
SELECT count(*) AS main_commits FROM dolt_log('main');
SELECT count(*) AS rows FROM app.t;
-- err(q): q's error, its hashes masked
CREATE FUNCTION pg_temp.err(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN regexp_replace(SQLERRM, '[0-9a-f]{64}', '<hash>', 'g');
END $$;
SELECT pg_temp.err(format('SELECT dolt_reset(%L, %L)', '--hard', :'side_head'));	-- collected
SELECT dolt_reset('--hard', :'kept_head');	-- settled, kept
SELECT count(*) AS rows FROM app.t;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_gc();
SELECT count(*) AS commits FROM dolt_log('main');

-- Privileges: the database's owner, or pg_maintain.
CREATE ROLE gc_user;
GRANT CREATE ON SCHEMA public TO gc_user;
SET ROLE gc_user;
SELECT dolt_gc();
RESET ROLE;
GRANT pg_maintain TO gc_user;
SET ROLE gc_user;
SELECT dolt_gc();
RESET ROLE;
SELECT dolt_gc('--nope');
-- (--shallow, Dolt's cheaper collection, collects what was written since)
SELECT dolt_gc('--shallow');
BEGIN READ ONLY;
SELECT dolt_gc();
ROLLBACK;

-- Not in a transaction that has used the store: it would straddle its own
-- swap (and wait on itself, had it appended).  One that hasn't may.
BEGIN;
SELECT test_chunk_put('gc used first') IS NOT NULL AS put;
SELECT dolt_gc();
ROLLBACK;
BEGIN;
SELECT dolt_gc();
SELECT test_chunk_put('gc used after') IS NOT NULL AS put;
COMMIT;

\c postgres
DROP DATABASE gc_db;
DROP DATABASE gc_dolt;
DROP ROLE gc_user;
