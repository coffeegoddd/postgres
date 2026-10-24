-- A commit written from a canonical state already held (the refs'
-- commit_root()): its root and dependencies exactly those given, nothing
-- folded and no change log read, however dirty the working set, and no
-- branch moved.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

-- since(counter): pg_stat_prolly's counter's growth since it was last
-- asked, the session's counts flushed first (:flush, a statement of its
-- own).
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION pg_temp.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;

CREATE SCHEMA cr VERSIONED;
CREATE TABLE cr.t (id int PRIMARY KEY, v text);
INSERT INTO cr.t SELECT g, 'v' || g FROM generate_series(1, 500) g;
SELECT (dolt_commit('-Am', 'first'))[1] AS c1 \gset

-- A dirty working set: any fold would read these rows.
UPDATE cr.t SET v = 'dirty' WHERE id <= 200;
CREATE TABLE cr.u (a int PRIMARY KEY);
:flush
SELECT pg_temp.since(c) IS NOT NULL AS counting
FROM unnest(ARRAY['fold_rows_read', 'fold_tables', 'fold_pages',
				  'log_pages_read', 'fold_catalog_scans', 'commits']) c;

SELECT test_commit_root(:'c1', :'c1') AS rooted \gset
:flush
SELECT c AS counter, pg_temp.since(c) AS grew
FROM unnest(ARRAY['fold_rows_read', 'fold_tables', 'fold_pages',
				  'log_pages_read', 'fold_catalog_scans', 'commits']) c;

-- Its root and dependencies are the ones given; it is a new commit, whose
-- parent is the one given.
SELECT :'rooted' <> :'c1' AS new_commit;
SELECT (SELECT r FROM test_commit_roots(:'rooted') r)
	 = (SELECT r FROM test_commit_roots(:'c1') r) AS same_roots;
SELECT dolt_branch('rooted', :'rooted');
SELECT dolt_hashof('rooted~1') = :'c1' AS parent_given;
SELECT message FROM dolt_log('rooted') LIMIT 1;

-- No branch moved, and the working set is as it was.
SELECT dolt_hashof('HEAD') = :'c1' AS head_unmoved;
SELECT table_name, status FROM dolt_status ORDER BY 1;

-- The commit reads as the root given: c1's rows.
SELECT dolt_reset('--hard');
SELECT dolt_checkout('rooted');
SELECT count(*) AS rows, count(*) FILTER (WHERE v = 'dirty') AS dirty FROM cr.t;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'rooted');
DROP SCHEMA cr CASCADE;
