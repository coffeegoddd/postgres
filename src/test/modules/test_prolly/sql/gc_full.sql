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

-- (This store's counts as each step left them, to show which step wrote
-- what when a count below isn't the one expected: printed only then.
-- #1020: the counts have come out otherwise now and then, with another
-- suite running beside.)
CREATE TEMP TABLE steps (n serial, step text, counts jsonb);
CREATE FUNCTION pg_temp.note(step text) RETURNS bool LANGUAGE sql AS $$
	INSERT INTO steps (step, counts)
	SELECT step, jsonb_object_agg(name, value) FROM pg_stat_prolly;
	SELECT true
$$;
\set diverged false
-- (each file a collection wrote, as it wrote it: whether every chunk in
-- it was one the refs' root or the tables reached as the collection began
-- or reach as it ends, found apart from its marks (test_chunk_reachable():
-- the collection moves the refs once it has copied what it keeps, so refs
-- nodes it copied may be replaced by then), so none it kept was garbage.
-- How many chunks a file holds depends on where chunk boundaries fall,
-- which move with the transaction ids and commit hashes the refs hold
-- (#1020), so its chunks are checked against what is live, not counted.)
CREATE TEMP TABLE live_at_start (n text);
CREATE FUNCTION pg_temp.collecting() RETURNS bool LANGUAGE sql AS $$
	DELETE FROM live_at_start;
	INSERT INTO live_at_start SELECT DISTINCT n FROM test_chunk_reachable() n;
	SELECT true
$$;
CREATE TEMP TABLE written (step text, name text, all_live bool);
CREATE FUNCTION pg_temp.wrote(step text) RETURNS bool LANGUAGE sql AS $$
	INSERT INTO written
	SELECT wrote.step, f.name,
		NOT EXISTS (SELECT FROM test_chunk_file_hashes(f.name) h
					WHERE h NOT IN (SELECT n FROM live_at_start) AND
						  h NOT IN (SELECT n FROM test_chunk_reachable() n))
	FROM (SELECT name FROM test_chunk_table_files() ORDER BY name DESC LIMIT 1) f;
	SELECT true
$$;
-- (the store's files by name, each settled, and live as written)
CREATE FUNCTION pg_temp.files() RETURNS TABLE (name text, live_as_written bool, settled bool)
LANGUAGE sql AS $$
	SELECT f.name, w.all_live, f.settled
	FROM test_chunk_table_files() f LEFT JOIN written w ON w.name = f.name
	ORDER BY f.name
$$;
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('start') AS _n \gset

CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
INSERT INTO app.t SELECT g, 'main ' || g FROM generate_series(1, 20) g;
SELECT length(dolt_commit('-m', 'small main')::text) AS committed;
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('small main') AS _n \gset

-- A big branch, settled by a collection, then deleted.
SELECT dolt_branch('big');
SELECT dolt_checkout('big');
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('checkout big') AS _n \gset
INSERT INTO app.t SELECT g, repeat('big', 50) || g FROM generate_series(21, 3000) g;
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('insert on big') AS _n \gset
SELECT length(dolt_commit('-m', 'on big')::text) AS committed;
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('commit on big') AS _n \gset
SELECT dolt_hashof('big') AS big_head \gset
SELECT dolt_checkout('main');
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('checkout main') AS _n \gset
SELECT dolt_gc();
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('gc 1') AS _n \gset
SELECT count(*) AS files, bool_and(settled) AS settled FROM test_chunk_table_files();
-- (what the refs keep alive before the delete)
CREATE TEMP TABLE live_before AS SELECT DISTINCT n FROM test_refs_walk_nodes() n;
SELECT dolt_branch('-D', 'big');
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('branch -D big') AS _n \gset

-- A default collection keeps it: it is settled.  It reads only the chunks
-- appended since the first collection settled the rest, the delete's:
-- it keeps those the refs reach now, the delete's new refs nodes, and
-- frees the others, refs nodes the delete wrote and replaced again as it
-- went (each entry it removes rewrites the path to it).  How many of each
-- depends on where the refs tree's chunk boundaries fall, which move with
-- the transaction ids and commit hashes its entries hold, so they are
-- measured, not fixed (#1020): those reached now and not before, and the
-- chunks the delete appended.
SELECT count(DISTINCT n) AS new_live FROM test_refs_walk_nodes() n
WHERE n NOT IN (SELECT n FROM live_before) \gset
SELECT (a.counts ->> 'chunks_appended')::int - (b.counts ->> 'chunks_appended')::int
	AS appended
FROM steps a, steps b WHERE a.step = 'branch -D big' AND b.step = 'gc 1' \gset
SELECT pg_temp.collecting() AS _c \gset
SELECT kept, freed FROM test_chunk_gc(false) \gset gc_
SELECT pg_temp.wrote('default collection') AS _w \gset
SELECT :gc_kept = :new_live AS kept_the_new_refs,
	   :gc_freed = :appended - :new_live AS freed_the_rest,
	   :gc_freed > 0 AS freed_some;
SELECT :'diverged'::bool OR :gc_kept <> :new_live OR
	:gc_freed <> :appended - :new_live AS diverged \gset
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('default collection') AS _n \gset
SELECT test_chunk_has(:'big_head') AS big_head;

-- A full one frees it: the file is mostly garbage, so rewritten.
SELECT name AS big_file FROM test_chunk_table_files()
ORDER BY chunks DESC, name LIMIT 1 \gset
SELECT pg_temp.collecting() AS _c \gset
SELECT kept > 0 AS kept, freed > 10 AS freed, bytes_freed > 10000 AS bytes_freed
FROM test_chunk_gc(true);
SELECT pg_temp.wrote('full collection') AS _w \gset
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('full collection') AS _n \gset
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
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('insert more') AS _n \gset
SELECT length(dolt_commit('-m', 'more on main')::text) AS committed;
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('commit more') AS _n \gset
SELECT pg_temp.collecting() AS _c \gset
SELECT dolt_gc();
SELECT pg_temp.wrote('gc 2') AS _w \gset
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('gc 2') AS _n \gset
SELECT name AS live_file FROM test_chunk_table_files()
ORDER BY chunks DESC, name LIMIT 1 \gset
UPDATE app.t SET v = 'changed' WHERE id = 7;
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('update') AS _n \gset
SELECT length(dolt_commit('-m', 'a change')::text) AS committed;
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('commit change') AS _n \gset
SELECT pg_temp.collecting() AS _c \gset
SELECT dolt_gc();
SELECT pg_temp.wrote('gc 3') AS _w \gset
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('gc 3') AS _n \gset
-- (Which files it keeps, by name: the mostly live ones kept whole, none
-- rewritten, the one wholly garbage gone.)
SELECT * FROM pg_temp.files();
SELECT :'diverged'::bool OR NOT (SELECT bool_and(live_as_written) FROM pg_temp.files())
	AS diverged \gset
SELECT freed > 0 AS freed FROM test_chunk_gc(true);
SELECT pg_stat_force_next_flush() AS _f \gset
SELECT pg_temp.note('full collection 2') AS _n \gset
SELECT count(*) AS live_file_kept_whole FROM test_chunk_table_files()
WHERE name = :'live_file';
SELECT * FROM pg_temp.files();
SELECT count(*) AS rows FROM app.t;
SELECT message FROM dolt_log('main');
-- (what each step counted, if a count above isn't the expected one)
\if :diverged
SELECT s.n, s.step, d.key AS counter, d.value::int8 - (p.counts ->> d.key)::int8 AS grew
FROM steps s JOIN steps p ON p.n = s.n - 1, jsonb_each_text(s.counts) d
WHERE d.value::int8 <> coalesce((p.counts ->> d.key)::int8, 0)
  AND (d.key IN ('chunks_appended', 'nodes_written', 'refs_nodes_written',
				 'value_nodes_written', 'commits', 'head_moves', 'move_plans',
				 'manifest_bytes_written', 'table_file_bytes_written')
	   OR d.key LIKE 'fold\_%' OR d.key LIKE 'gc\_%')
ORDER BY s.n, d.key;
\endif

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
