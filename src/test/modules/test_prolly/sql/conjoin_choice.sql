-- Conjoining past a number of table files rewrites only the smallest, as
-- Dolt's conjoiner chooses them: the two smallest, then each next while
-- those taken add up to more than it, or while leaving it would leave too
-- many files.  Settled files and the others are conjoined apart, so what
-- was settled stays settled.  A chunk two files hold is written once.
\c postgres
CREATE DATABASE conjoin_db;
\c conjoin_db
CREATE EXTENSION test_prolly;
CREATE FUNCTION files() RETURNS TABLE (chunks int, settled bool) LANGUAGE sql AS $$
	SELECT chunks, settled FROM test_chunk_table_files() ORDER BY settled, chunks
$$;
-- A file of n chunks, named by tag.
CREATE FUNCTION add_file(tag text, n int) RETURNS void LANGUAGE sql AS $$
	SELECT test_chunk_table_file(array_agg(test_noise(tag || g, 100)))
	FROM generate_series(1, n) g
$$;
SELECT add_file('a', 1), add_file('b', 1), add_file('c', 2), add_file('d', 3),
	add_file('e', 8), add_file('f', 20);
SELECT * FROM files();

-- Under the limit, nothing.
SELECT test_chunk_conjoin(6);
SELECT count(*) FROM files();

-- Past it: 1 and 1 (2), then 2 (2 is no more than it, but leaving it
-- leaves 5 files, past 4), then 3 (4 add up to more); then 8, which the
-- 7 taken add up to no more than, with 3 files left: stop.
SELECT test_chunk_conjoin(4);
SELECT * FROM files();
SELECT count(*) AS readable FROM (VALUES ('a', 1), ('b', 1), ('c', 2), ('d', 3), ('e', 8), ('f', 20)) f(tag, n),
	generate_series(1, 20) g
WHERE g <= n AND test_chunk_has(test_content_hash(test_noise(tag || g, 100)));

-- Settled files stay apart: a collection settles what the root reaches,
-- then four files not settled come, and conjoining past 3 takes from them.
SELECT test_chunk_put_node(1, ARRAY[test_content_hash(test_noise('a1', 100)),
									test_content_hash(test_noise('f1', 100))]) AS root \gset
SELECT test_chunk_move_root(NULL, :'root');
SELECT kept FROM test_chunk_gc(false);
SELECT * FROM files();
SELECT add_file('g', 1), add_file('h', 1), add_file('i', 2), add_file('j', 2);
SELECT test_chunk_conjoin(3);
SELECT * FROM files();

-- A chunk two files hold is in the conjoined file once.
SELECT add_file('k', 3);
SELECT test_chunk_table_file(ARRAY[test_noise('k1', 100), test_noise('k2', 100)]);
SELECT test_chunk_conjoin();
SELECT * FROM files();

\c postgres
DROP DATABASE conjoin_db;
