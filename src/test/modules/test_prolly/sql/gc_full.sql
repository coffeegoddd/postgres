-- A full collection: every file collected, a settled file whose garbage
-- is more than half rewritten, the rest kept whole.  What a default
-- collection keeps (a branch deleted after its chunks settled) a full one
-- frees.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE DATABASE gc_full_db;
\c gc_full_db
CREATE EXTENSION test_prolly;

-- err(q): q's error, its hashes masked
CREATE FUNCTION pg_temp.err(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN regexp_replace(SQLERRM, '[0-9a-f]{40}', '<hash>', 'g');
END $$;

CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
INSERT INTO app.t SELECT g, 'main ' || g FROM generate_series(1, 20) g;
SELECT length(dolt_commit('-m', 'small main')::text) AS committed;

-- A big branch, settled by a collection, then deleted.
SELECT dolt_branch('big');
SELECT dolt_checkout('big');
INSERT INTO app.t SELECT g, repeat('big', 50) || g FROM generate_series(21, 3000) g;
SELECT length(dolt_commit('-m', 'on big')::text) AS committed;
SELECT dolt_hashof('big') AS big_head \gset
SELECT dolt_checkout('main');
SELECT dolt_gc();
SELECT count(*) AS files, bool_and(settled) AS settled FROM test_chunk_table_files();
SELECT dolt_branch('-D', 'big');

-- A default collection keeps it: it is settled.
SELECT kept, freed FROM test_chunk_gc(false);
SELECT test_chunk_has(:'big_head') AS big_head;

-- A full one frees it: the file is mostly garbage, so rewritten.
SELECT name AS big_file FROM test_chunk_table_files()
ORDER BY chunks DESC, name LIMIT 1 \gset
SELECT kept > 0 AS kept, freed > 10 AS freed, bytes_freed > 10000 AS bytes_freed
FROM test_chunk_gc(true);
SELECT test_chunk_has(:'big_head') AS big_head;
SELECT count(*) FILTER (WHERE name = :'big_file') AS big_file_left,
	bool_and(settled) AS settled
FROM test_chunk_table_files();
SELECT pg_temp.err(format('SELECT dolt_reset(%L, %L)', '--hard', :'big_head'));
SELECT count(*) AS rows FROM app.t;
SELECT message FROM dolt_log('main');

-- A mostly live store: a full collection keeps a settled file whole if
-- at most half of it is garbage.
INSERT INTO app.t SELECT g, repeat('more', 50) || g FROM generate_series(21, 3000) g;
SELECT length(dolt_commit('-m', 'more on main')::text) AS committed;
SELECT dolt_gc();
SELECT name AS live_file FROM test_chunk_table_files()
ORDER BY chunks DESC, name LIMIT 1 \gset
UPDATE app.t SET v = 'changed' WHERE id = 7;
SELECT length(dolt_commit('-m', 'a change')::text) AS committed;
SELECT dolt_gc();
-- (Which files it keeps, by name: the mostly live ones kept whole, none
-- rewritten, the one wholly garbage gone.)
SELECT name, chunks, settled FROM test_chunk_table_files() ORDER BY name;
SELECT freed > 0 AS freed FROM test_chunk_gc(true);
SELECT count(*) AS live_file_kept_whole FROM test_chunk_table_files()
WHERE name = :'live_file';
SELECT name, chunks, settled FROM test_chunk_table_files() ORDER BY name;
SELECT count(*) AS rows FROM app.t;
SELECT message FROM dolt_log('main');

-- dolt_gc('--full') runs it.
SELECT dolt_gc('--full');
SELECT count(*) AS rows FROM app.t;
\c gc_full_db
SELECT count(*) AS rows FROM app.t;
SELECT message FROM dolt_log('main');

-- A file kept whole keeps what its chunks name, live or not: the store
-- stays closed under what its chunks name.
\c postgres
CREATE DATABASE gc_closure_db;
\c gc_closure_db
CREATE EXTENSION test_prolly;
SELECT test_chunk_put('h') AS h \gset
SELECT array_agg(test_chunk_put(convert_to('junk ' || g, 'UTF8'))) AS junk
FROM generate_series(1, 5) g \gset
SELECT test_chunk_put_node(1, ARRAY[:'h'] || :'junk'::text[]) AS root0 \gset
SELECT test_chunk_move_root(NULL, :'root0');
SELECT kept FROM test_chunk_gc(false);
SELECT array_agg(test_chunk_put(convert_to('live ' || g, 'UTF8'))) AS live
FROM generate_series(1, 10) g \gset
SELECT test_chunk_put_node(1, ARRAY[:'h']) AS g \gset
SELECT test_chunk_put_node(1, :'live'::text[] || ARRAY[:'g']) AS root1 \gset
SELECT test_chunk_move_root(:'root0', :'root1');
SELECT kept FROM test_chunk_gc(false);
SELECT test_chunk_put_node(1, :'live'::text[]) AS root2 \gset
SELECT test_chunk_move_root(:'root1', :'root2');
SELECT count(*) AS files, sum(chunks) AS chunks FROM test_chunk_table_files();
SELECT kept, freed FROM test_chunk_gc(true);
SELECT test_chunk_has(:'g') AS g_kept_with_its_file, test_chunk_has(:'h') AS h_it_names,
	test_chunk_has((:'junk'::text[])[1]) AS junk, test_chunk_has(:'root0') AS root0;

-- Which files are kept is chosen again once a kept file's garbage has
-- marked what it names: file B, all garbage from the root, is half live
-- through what kept file A's garbage names, so it is kept whole too.
\c postgres
CREATE DATABASE gc_again_db;
\c gc_again_db
CREATE EXTENSION test_prolly;
SELECT array_agg(test_chunk_put(convert_to('b ' || g, 'UTF8'))) AS bs
FROM generate_series(1, 10) g \gset
SELECT test_chunk_put_node(1, :'bs'::text[]) AS root_b \gset
SELECT test_chunk_move_root(NULL, :'root_b');
SELECT kept FROM test_chunk_gc(false);
SELECT name AS file_b FROM test_chunk_table_files() \gset
SELECT array_agg(test_chunk_put(convert_to('a ' || g, 'UTF8'))) AS as_
FROM generate_series(1, 10) g \gset
SELECT test_chunk_put_node(1, (:'bs'::text[])[1:6]) AS names_b \gset
SELECT test_chunk_put_node(1, :'as_'::text[] || ARRAY[:'names_b']) AS root_a \gset
SELECT test_chunk_move_root(:'root_b', :'root_a');
SELECT kept FROM test_chunk_gc(false);
SELECT test_chunk_put_node(1, :'as_'::text[]) AS root_c \gset
SELECT test_chunk_move_root(:'root_a', :'root_c');
SELECT kept, freed FROM test_chunk_gc(true);
SELECT count(*) FILTER (WHERE name = :'file_b') AS file_b_kept_whole,
	count(*) AS files
FROM test_chunk_table_files();
SELECT test_chunk_has((:'bs'::text[])[1]) AS named_from_kept,
	test_chunk_has(:'root_c') AS root;

\c postgres
DROP DATABASE gc_full_db;
DROP DATABASE gc_closure_db;
DROP DATABASE gc_again_db;
