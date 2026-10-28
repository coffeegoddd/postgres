-- A collection or a retire run by a function inside a query, while the
-- query's scan of a table holds a pin on a page dirtied before: the
-- collection writes out the database's dirty pages, that one too, through
-- the pin the scan holds rather than a second one of its own.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE DATABASE gc_scan_pins_db;
\c gc_scan_pins_db
CREATE EXTENSION test_prolly;

-- A generation to retire.
SELECT test_chunk_put('pins a1') AS a1 \gset
SELECT test_chunk_put('pins a2') AS a2 \gset
SELECT test_chunk_put_node(1, ARRAY[:'a1', :'a2']) AS na \gset
SELECT test_chunk_move_root(NULL, :'na');
SELECT kept, freed FROM test_chunk_gc(false);
SELECT test_chunk_generation() AS ga \gset

-- (heap tables: their pages are in shared buffers, dirty, when the scans
-- below read them)
CREATE TABLE gens (n int) USING heap;
INSERT INTO gens VALUES (1), (2);
CREATE TABLE retired (n int, kept int8) USING heap;

-- The retire inside a join over a heap scan (the scan of gens is the
-- outer side, so it holds its page's pin through each call).
INSERT INTO retired
	SELECT g.n, r.kept FROM gens g, LATERAL test_chunk_retire(:ga + 0 * g.n, :ga) r;
SELECT n, kept IS NOT NULL AS retired FROM retired ORDER BY n;
SELECT test_chunk_has(:'a1') AS a1, test_chunk_has(:'a2') AS a2;

\c postgres
DROP DATABASE gc_scan_pins_db;

-- A default and a full collection, and dolt_gc(), each called per row of a
-- scan of a page just dirtied, in a store of commits.
CREATE DATABASE gc_scan_pins_db;
\c gc_scan_pins_db
CREATE TABLE t (id int PRIMARY KEY, v text);
INSERT INTO t SELECT g, 'v' || g FROM generate_series(1, 100) g;
SELECT dolt_commit('-Am', 'one') IS NOT NULL AS committed;
UPDATE t SET v = v || '!' WHERE id <= 10;
CREATE TABLE gens (n int) USING heap;
INSERT INTO gens VALUES (1), (2);
SELECT n, (SELECT chunks_kept IS NOT NULL FROM prolly_gc(false)) AS collected FROM gens ORDER BY n;
UPDATE gens SET n = n + 10;
SELECT n, (SELECT chunks_kept IS NOT NULL FROM prolly_gc(true)) AS collected FROM gens ORDER BY n;
UPDATE gens SET n = n + 10;
-- (one row: a transaction that has used the store can't run another)
SELECT n, dolt_gc() IS NOT NULL AS collected FROM gens WHERE n = 21;

-- The pages written are the scans' latest, and the store still reads.
CHECKPOINT;
SELECT n FROM gens ORDER BY n;
SELECT count(*), count(*) FILTER (WHERE v LIKE '%!') AS changed FROM t;
SELECT count(*) AS commits FROM dolt_log;

\c postgres
DROP DATABASE gc_scan_pins_db;
