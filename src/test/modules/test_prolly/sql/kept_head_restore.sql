-- A hard reset to a commit whose kept pages are gone, from a working set
-- with many changes, gives each table the pages kept as of the head first
-- (O(1), as a reset to the head does), then writes only the rows the head
-- and the commit differ by: not those and every row changed since the
-- head.  That is the reset a commit no longer kept takes: here, with only
-- the head kept (prolly.kept_commits = 1), every commit reset away from.
-- (A restore carries the other kept commits' snapshots into the storage it
-- gives a table, so with more kept they stay kept.)

SELECT current_database() AS home \gset
CREATE DATABASE kept_head_restore;
\c kept_head_restore
SET client_min_messages = warning;
CREATE EXTENSION amcheck;
RESET client_min_messages;

-- (the counters a move counts, read once the session's are flushed)
CREATE FUNCTION counted() RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$
	SELECT n, coalesce((SELECT p.value FROM pg_stat_prolly p WHERE p.name = n), -1)
	FROM unnest('{apply_rows,relations_restored}'::text[]) n
$$;
CREATE FUNCTION since(before jsonb) RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(c.name, c.value - (before ->> c.name)::bigint)
	FROM counted() c
$$;
CREATE FUNCTION now_counted() RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(name, value) FROM counted()
$$;
SET prolly.kept_commits = 1;
CREATE SCHEMA kh VERSIONED;
CREATE TABLE kh.t (id bigint PRIMARY KEY, v int NOT NULL, w text NOT NULL);
CREATE INDEX t_v ON kh.t (v);
INSERT INTO kh.t SELECT g, 0, 'w' || g FROM generate_series(1, 5000) g;
CREATE FUNCTION indexes_valid() RETURNS bool LANGUAGE sql AS $$
	SELECT bool_and(bt_index_check(i.indexrelid, true) IS NOT NULL)
	FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'btree'
	WHERE i.indrelid IN (SELECT oid FROM pg_class
						 WHERE relnamespace = 'kh'::regnamespace)
$$;
CREATE FUNCTION rows_digest() RETURNS text LANGUAGE sql AS $$
	SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM kh.t
$$;
CREATE FUNCTION clean() RETURNS bool LANGUAGE sql AS
$$SELECT NOT EXISTS (SELECT FROM dolt_status)$$;

SELECT (dolt_commit('-Am', 'a'))[1] AS a \gset
SELECT rows_digest() AS at_a \gset
-- b: 50 rows changed
UPDATE kh.t SET v = 1 WHERE id % 100 = 0;
SELECT (dolt_commit('-am', 'b'))[1] AS b \gset
SELECT rows_digest() AS at_b \gset

-- Back and forth: to a, to b, to a again, so that b is no longer kept and
-- a, the head, is.
SELECT dolt_reset('--hard', :'a') IS NOT NULL AS reset;
SELECT dolt_reset('--hard', :'b') IS NOT NULL AS reset;
SELECT dolt_reset('--hard', :'a') IS NOT NULL AS reset;
SELECT rows_digest() = :'at_a' AS at_a, clean();

-- 1,000 rows changed in the working set, then a hard reset to b: the
-- head's pages given back, and only b's 50 rows written
UPDATE kh.t SET v = 2, w = w || 'x' WHERE id % 5 = 1;
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT dolt_reset('--hard', :'b') IS NOT NULL AS reset;
SELECT pg_stat_force_next_flush();
SELECT (since(:'before') ->> 'apply_rows')::int AS applied,
	   (since(:'before') ->> 'relations_restored')::int > 0 AS restored;
SELECT rows_digest() = :'at_b' AS at_b, clean(), indexes_valid();
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS at_v1 FROM kh.t WHERE v = 1;
SELECT count(*) AS at_v2 FROM kh.t WHERE v = 2;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- The same with the working set folded first (a checkpoint folds it, as the
-- fold worker does in the background): the baseline holds the working
-- set's rows, so it tells how far they are from b, and the head's pages
-- are given back as before.
SELECT dolt_reset('--hard', :'a') IS NOT NULL AS reset;
SELECT dolt_reset('--hard', :'b') IS NOT NULL AS reset;
SELECT dolt_reset('--hard', :'a') IS NOT NULL AS reset;
UPDATE kh.t SET v = 2, w = w || 'x' WHERE id % 5 = 1;
SELECT prolly_checkpoint() IS NOT NULL AS folded;
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT dolt_reset('--hard', :'b') IS NOT NULL AS reset;
SELECT pg_stat_force_next_flush();
SELECT (since(:'before') ->> 'apply_rows')::int AS applied,
	   (since(:'before') ->> 'relations_restored')::int > 0 AS restored;
SELECT rows_digest() = :'at_b' AS at_b, clean(), indexes_valid();

-- A working set a few rows from the commit (fewer than
-- prolly.restore_min_changes): the head's pages aren't given back, the
-- few rows written as they are
SELECT dolt_reset('--hard', :'a') IS NOT NULL AS reset;
SELECT dolt_reset('--hard', :'b') IS NOT NULL AS reset;
SELECT dolt_reset('--hard', :'a') IS NOT NULL AS reset;
UPDATE kh.t SET v = 1 WHERE id = 100;
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT dolt_reset('--hard', :'b') IS NOT NULL AS reset;
SELECT pg_stat_force_next_flush();
SELECT (since(:'before') ->> 'relations_restored')::int AS restored;
SELECT rows_digest() = :'at_b' AS at_b, clean(), indexes_valid();


-- A working set brought close to the commit by ordinary writes, its head
-- far from it: the head's pages are given back all the same, discarding
-- the working set's changes in O(1) without asking how far it is from the
-- commit, and the head's 10,000 rows from the commit are written (the
-- move's own distance, as a reset from a clean working set writes).
CREATE TABLE kh.n (id int PRIMARY KEY, v int);
INSERT INTO kh.n SELECT g, 0 FROM generate_series(1, 20000) g;
SELECT (dolt_commit('-Am', 'n a'))[1] AS na \gset
SELECT n_digest FROM (SELECT md5(string_agg(n::text, ',' ORDER BY id)) AS n_digest FROM kh.n n) x \gset
UPDATE kh.n SET v = 1 WHERE id <= 10000;
SELECT length(dolt_commit('-am', 'n b')::text) > 0 AS committed;
UPDATE kh.n SET v = 2 WHERE id <= 10;
SELECT length(dolt_commit('-am', 'n c')::text) > 0 AS committed;
UPDATE kh.n SET v = 0 WHERE id <= 10000;
UPDATE kh.n SET v = 9 WHERE id <= 5;
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT dolt_reset('--hard', :'na') IS NOT NULL AS reset;
SELECT pg_stat_force_next_flush();
SELECT (since(:'before') ->> 'apply_rows')::int AS applied,
	   (since(:'before') ->> 'relations_restored')::int AS restored;
SELECT md5(string_agg(n::text, ',' ORDER BY id)) = :'n_digest' AS at_na, clean(),
	indexes_valid()
FROM kh.n n;

-- With two commits kept: a reset to b, not kept, given the head's pages,
-- keeps b and the head (a's snapshot carried, or kept anew); a reset back
-- to a then gives a's pages back, writing no row.
SET prolly.kept_commits = 2;
DROP TABLE kh.n;
SELECT dolt_commit('-am', 'n dropped') IS NOT NULL AS committed;
SELECT dolt_reset('--hard', :'a') IS NOT NULL AS reset;
SELECT rows_digest() = :'at_a' AS at_a;
UPDATE kh.t SET v = 2, w = w || 'x' WHERE id % 5 = 1;
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT dolt_reset('--hard', :'b') IS NOT NULL AS reset;
SELECT pg_stat_force_next_flush();
SELECT (since(:'before') ->> 'apply_rows')::int AS applied,
	   (since(:'before') ->> 'relations_restored')::int > 0 AS restored;
SELECT rows_digest() = :'at_b' AS at_b, clean(), indexes_valid();
UPDATE kh.t SET v = 3 WHERE id % 7 = 1;
SELECT pg_stat_force_next_flush();
SELECT now_counted() AS before \gset
SELECT dolt_reset('--hard', :'a') IS NOT NULL AS reset;
SELECT pg_stat_force_next_flush();
SELECT (since(:'before') ->> 'apply_rows')::int AS applied,
	   (since(:'before') ->> 'relations_restored')::int > 0 AS restored;
SELECT rows_digest() = :'at_a' AS at_a, clean(), indexes_valid();
RESET prolly.kept_commits;

\c :home
DROP DATABASE kept_head_restore;
