-- A move that compares a diff's count with a bound counts the diff only up
-- to the bound: whether sharing another branch's pages costs less than
-- writing the rows that differ.  And a hard reset to a commit whose pages
-- aren't kept, from a dirty working set, gives the table the pages kept as
-- of the head whatever the working set holds, without counting how far it
-- is from the commit: the working set's changes are discarded in O(1), and
-- only the rows the head and the commit differ by (H) are written, however
-- many rows the working set changed (L) or its baseline holds (B), folded
-- or not.  Shown here case by case.

SELECT current_database() AS home \gset
CREATE DATABASE bounded_counts;
-- (history older than the window keeps no roots: a window long enough
-- for all of this one's)
ALTER DATABASE bounded_counts SET prolly.history_window = '1000 years';
\c bounded_counts

-- (the counters a move counts, read once the session's are flushed)
CREATE FUNCTION counted() RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(n, coalesce((SELECT p.value FROM pg_stat_prolly p
										 WHERE p.name = n), -1))
	FROM unnest('{apply_rows,relations_restored,nodes_read,move_tables_shared}'::text[]) n
$$;
CREATE FUNCTION since(before jsonb) RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(k, (counted() ->> k)::bigint - (before ->> k)::bigint)
	FROM jsonb_object_keys(before) k
$$;
CREATE FUNCTION clean() RETURNS bool LANGUAGE sql AS
$$SELECT NOT EXISTS (SELECT FROM dolt_status)$$;
\set flush 'SELECT pg_stat_force_next_flush();'

CREATE SCHEMA bc VERSIONED;
CREATE TABLE bc.t (id bigint PRIMARY KEY, v int NOT NULL, w text NOT NULL);
INSERT INTO bc.t SELECT g, 0, 'w' || g FROM generate_series(1, 100000) g;
CREATE FUNCTION t_digest() RETURNS text LANGUAGE sql AS $$
	SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM bc.t
$$;

-- Sharing: a branch whose working set is a commit c one row from a, and
-- this branch's working set a fifth of the rows from c, folded.  Writing
-- them costs more than sharing the other branch's pages, which the count
-- tells after a few rows.
SET debug_prolly_page_roots = off;
SELECT (dolt_commit('-Am', 'a'))[1] AS a \gset
SELECT dolt_branch('other') IS NOT NULL AS branched;
SELECT dolt_checkout('other') IS NOT NULL AS checked_out;
UPDATE bc.t SET v = 7 WHERE id = 1;
SELECT (dolt_commit('-am', 'c'))[1] AS c \gset
SELECT t_digest() AS at_c \gset
SELECT dolt_checkout('main') IS NOT NULL AS checked_out;
UPDATE bc.t SET v = 2, w = w || 'y' WHERE id % 5 = 2;
SELECT prolly_checkpoint() IS NOT NULL AS folded;
:flush
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'c') IS NOT NULL AS reset;
:flush
SELECT since(:'before') AS moved \gset
SELECT (:'moved'::jsonb ->> 'move_tables_shared')::int AS shared,
	   (:'moved'::jsonb ->> 'apply_rows')::int AS applied,
	   (:'moved'::jsonb ->> 'nodes_read')::int < 600 AS few_nodes_read;
SELECT t_digest() = :'at_c' AS at_c, clean();
SELECT dolt_branch('-D', 'other') IS NOT NULL AS deleted;

-- The head's pages: the head, now c, as a, and b 50 rows from it, all in
-- the first leaf.
RESET debug_prolly_page_roots;
ALTER DATABASE bounded_counts SET prolly.history_min_commits = 1;
ALTER DATABASE bounded_counts SET prolly.history_window = '1 microsecond';
SELECT :'c' AS a \gset
UPDATE bc.t SET v = 1 WHERE id <= 50;
SELECT (dolt_commit('-am', 'b'))[1] AS b \gset
SELECT t_digest() AS at_b \gset

-- Back and forth, so that b is no longer kept and a, the head, is; then a
-- fifth of the rows changed, spread over every leaf, and folded.
SELECT dolt_reset('--hard', :'a') IS NOT NULL AS reset;
SELECT dolt_reset('--hard', :'b') IS NOT NULL AS reset;
SELECT dolt_reset('--hard', :'a') IS NOT NULL AS reset;
UPDATE bc.t SET v = 2, w = w || 'x' WHERE id % 5 = 1;
SELECT prolly_checkpoint() IS NOT NULL AS folded;
:flush
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'b') IS NOT NULL AS reset;
:flush
SELECT since(:'before') AS moved \gset
SELECT (:'moved'::jsonb ->> 'relations_restored')::int > 0 AS restored,
	   (:'moved'::jsonb ->> 'apply_rows')::int AS applied,
	   (:'moved'::jsonb ->> 'nodes_read')::int < 600 AS few_nodes_read;
SELECT t_digest() = :'at_b' AS at_b, clean();

DROP TABLE bc.t;
SELECT dolt_commit('-Am', 't dropped') IS NOT NULL AS committed;

-- L at least B - H and both large: the head 20,000 rows from y, 60,000
-- more rows folded, and 61,000 changed since.  The head's pages are given
-- back, and only H's 20,000 rows written, not the 81,000 the working set
-- is from y (the working set's rows, neither folded nor counted).
CREATE TABLE bc.l (id int PRIMARY KEY, v int NOT NULL, w text NOT NULL);
INSERT INTO bc.l SELECT g, 0, repeat(md5(g::text), 3)
FROM generate_series(1, 100000) g;
CREATE FUNCTION l_digest() RETURNS text LANGUAGE sql AS $$
	SELECT md5(string_agg(l::text, ',' ORDER BY id)) FROM bc.l l
$$;
SELECT (dolt_commit('-Am', 'l x'))[1] AS lx \gset
UPDATE bc.l SET v = 1 WHERE id <= 20000;
SELECT (dolt_commit('-am', 'l y'))[1] AS ly \gset
SELECT l_digest() AS at_ly \gset
SELECT dolt_reset('--hard', :'lx') IS NOT NULL AS reset;
SELECT dolt_reset('--hard', :'ly') IS NOT NULL AS reset;
SELECT dolt_reset('--hard', :'lx') IS NOT NULL AS reset;
UPDATE bc.l SET v = 2 WHERE id > 40000;
SELECT prolly_checkpoint() IS NOT NULL AS folded;
UPDATE bc.l SET v = 3 WHERE id > 20000 AND id <= 81000;
:flush
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'ly') IS NOT NULL AS reset;
:flush
SELECT since(:'before') AS moved \gset
SELECT (:'moved'::jsonb ->> 'relations_restored')::int AS restored,
	   (:'moved'::jsonb ->> 'apply_rows')::int AS applied,
	   (:'moved'::jsonb ->> 'nodes_read')::int < 26000 AS few_nodes_read;
SELECT l_digest() = :'at_ly' AS at_ly, clean();
DROP TABLE bc.l;
SELECT dolt_commit('-Am', 'l dropped') IS NOT NULL AS committed;

-- The decision, case by case, on a table of 20,000 rows.  Each case makes
-- a commit y from the head x, resets to x, y and x again (y no longer
-- kept, x kept), changes the working set, and resets to y.
CREATE TABLE bc.d (id int PRIMARY KEY, v int NOT NULL);
INSERT INTO bc.d SELECT g, 0 FROM generate_series(1, 20000) g;
CREATE FUNCTION d_digest() RETURNS text LANGUAGE sql AS $$
	SELECT md5(string_agg(d::text, ',' ORDER BY id)) FROM bc.d d
$$;
SELECT (dolt_commit('-Am', 'd'))[1] AS x \gset
CREATE FUNCTION commit_pair(n int, label text) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
	UPDATE bc.d SET v = v + 1 WHERE id <= n;
	RETURN (dolt_commit('-am', label))[1];
END
$$;
CREATE FUNCTION back_and_forth(x text, y text) RETURNS bool
LANGUAGE sql AS $$
	SELECT dolt_reset('--hard', x) IS NOT NULL AND
		dolt_reset('--hard', y) IS NOT NULL AND
		dolt_reset('--hard', x) IS NOT NULL
$$;

-- B small: the working set brought to y but for 5 rows and folded; the
-- head is 10,000 rows from y, more than the baseline is.  Restored all
-- the same: the working set's changes are discarded, and the head's
-- 10,000 rows written (the move's own distance, whatever the working set
-- holds).
SELECT commit_pair(10000, 'y1') AS y \gset
SELECT d_digest() AS at_y \gset
SELECT back_and_forth(:'x', :'y');
UPDATE bc.d SET v = v + 1 WHERE id <= 10000;
UPDATE bc.d SET v = 9 WHERE id <= 5;
SELECT prolly_checkpoint() IS NOT NULL AS folded;
:flush
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'y') IS NOT NULL AS reset;
:flush
SELECT (since(:'before') ->> 'relations_restored')::int > 0 AS restored,
	   (since(:'before') ->> 'apply_rows')::int AS applied,
	   d_digest() = :'at_y' AS at_y, clean();
SELECT :'y' AS x \gset

-- B big, L small: 6,000 rows changed and folded, 100 more since; the head
-- 50 rows from y.  Restored: the 50 rows written.
SELECT commit_pair(50, 'y2') AS y \gset
SELECT d_digest() AS at_y \gset
SELECT back_and_forth(:'x', :'y');
UPDATE bc.d SET v = v + 7 WHERE id > 10000 AND id <= 16000;
SELECT prolly_checkpoint() IS NOT NULL AS folded;
UPDATE bc.d SET v = v + 7 WHERE id > 16000 AND id <= 16100;
:flush
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'y') IS NOT NULL AS reset;
:flush
SELECT (since(:'before') ->> 'relations_restored')::int > 0 AS restored,
	   (since(:'before') ->> 'apply_rows')::int AS applied,
	   d_digest() = :'at_y' AS at_y, clean();
SELECT :'y' AS x \gset

-- L big: 6,000 rows changed and folded, then 7,000 more since, more than
-- B less H.  Restored: the 50 rows written.
SELECT commit_pair(50, 'y3') AS y \gset
SELECT d_digest() AS at_y \gset
SELECT back_and_forth(:'x', :'y');
UPDATE bc.d SET v = v + 7 WHERE id > 10000 AND id <= 16000;
SELECT prolly_checkpoint() IS NOT NULL AS folded;
UPDATE bc.d SET v = v + 7 WHERE id > 1000 AND id <= 8000;
:flush
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'y') IS NOT NULL AS reset;
:flush
SELECT (since(:'before') ->> 'relations_restored')::int > 0 AS restored,
	   (since(:'before') ->> 'apply_rows')::int AS applied,
	   d_digest() = :'at_y' AS at_y, clean();
SELECT :'y' AS x \gset

-- L large but less than B less H: 6,000 rows folded, 5,000 since.
-- Restored.
SELECT commit_pair(50, 'y4') AS y \gset
SELECT d_digest() AS at_y \gset
SELECT back_and_forth(:'x', :'y');
UPDATE bc.d SET v = v + 7 WHERE id > 10000 AND id <= 16000;
SELECT prolly_checkpoint() IS NOT NULL AS folded;
UPDATE bc.d SET v = v + 7 WHERE id > 1000 AND id <= 6000;
:flush
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'y') IS NOT NULL AS reset;
:flush
SELECT (since(:'before') ->> 'relations_restored')::int > 0 AS restored,
	   (since(:'before') ->> 'apply_rows')::int AS applied,
	   d_digest() = :'at_y' AS at_y, clean();
SELECT :'y' AS x \gset

-- B and L both large, L less than B less H: 10,000 rows folded, 5,000
-- since.  Restored.
SELECT commit_pair(50, 'y5') AS y \gset
SELECT d_digest() AS at_y \gset
SELECT back_and_forth(:'x', :'y');
UPDATE bc.d SET v = v + 7 WHERE id > 10000;
SELECT prolly_checkpoint() IS NOT NULL AS folded;
UPDATE bc.d SET v = v + 7 WHERE id > 1000 AND id <= 6000;
:flush
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'y') IS NOT NULL AS reset;
:flush
SELECT (since(:'before') ->> 'relations_restored')::int > 0 AS restored,
	   (since(:'before') ->> 'apply_rows')::int AS applied,
	   d_digest() = :'at_y' AS at_y, clean();
SELECT :'y' AS x \gset

-- The baseline holding the head's rows (nothing folded since the reset):
-- the head 5,000 rows from y, and 12,000 rows changed, at least twice as
-- many.  Restored.
SELECT commit_pair(5000, 'y6') AS y \gset
SELECT d_digest() AS at_y \gset
SELECT back_and_forth(:'x', :'y');
UPDATE bc.d SET v = v + 3 WHERE id > 8000;
:flush
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'y') IS NOT NULL AS reset;
:flush
SELECT (since(:'before') ->> 'relations_restored')::int > 0 AS restored,
	   (since(:'before') ->> 'apply_rows')::int AS applied,
	   d_digest() = :'at_y' AS at_y, clean();
SELECT :'y' AS x \gset

-- The same with 9,000 rows changed, fewer than twice 5,000.  Restored:
-- the 5,000 rows written.
SELECT commit_pair(5000, 'y7') AS y \gset
SELECT d_digest() AS at_y \gset
SELECT back_and_forth(:'x', :'y');
UPDATE bc.d SET v = v + 3 WHERE id > 11000;
:flush
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'y') IS NOT NULL AS reset;
:flush
SELECT (since(:'before') ->> 'relations_restored')::int > 0 AS restored,
	   (since(:'before') ->> 'apply_rows')::int AS applied,
	   d_digest() = :'at_y' AS at_y, clean();
SELECT :'y' AS x \gset

-- And with 1,000 rows changed, the head 50 rows from y.  Restored.
SELECT commit_pair(50, 'y8') AS y \gset
SELECT d_digest() AS at_y \gset
SELECT back_and_forth(:'x', :'y');
UPDATE bc.d SET v = v + 3 WHERE id > 19000;
:flush
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', :'y') IS NOT NULL AS reset;
:flush
SELECT (since(:'before') ->> 'relations_restored')::int > 0 AS restored,
	   (since(:'before') ->> 'apply_rows')::int AS applied,
	   d_digest() = :'at_y' AS at_y, clean();
ALTER DATABASE bounded_counts RESET prolly.history_min_commits;
ALTER DATABASE bounded_counts SET prolly.history_window = '1000 years';

\c :home
DROP DATABASE bounded_counts;
