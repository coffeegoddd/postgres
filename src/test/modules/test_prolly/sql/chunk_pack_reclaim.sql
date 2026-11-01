-- A pack's chunks that fit a page go into pages freed, as a put's do: room
-- is made a chunk at a time.  (A live chunk past the pages freed keeps
-- them in the log's middle, which VACUUM would otherwise cut off.)
CREATE EXTENSION IF NOT EXISTS test_prolly;
SET chunk_free_in_background = off;
CREATE FUNCTION pr_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS 'SELECT substr(sha256(d), 1, 20)';
CREATE FUNCTION pr_blob(tag text, i int) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT 'L'::bytea || decode(repeat(md5(tag || i), 25), 'hex')$$;
-- pages of chunks, then one kept past them; the chunks freed
SELECT test_cs_put_many('pages', ARRAY(SELECT pr_h(pr_blob('pr-a', i)) FROM generate_series(1, 400) i ORDER BY i),
	ARRAY(SELECT pr_blob('pr-a', i) FROM generate_series(1, 400) i ORDER BY i)) AS stored;
\c
SET chunk_free_in_background = off;
SELECT (test_cs_put('pages', pr_blob('pr-keep', 1))).stored AS kept;
SELECT test_cs_ref_root('pages', pr_h(pr_blob('pr-keep', 1)));
SELECT test_cs_free('pages', ARRAY(SELECT pr_h(pr_blob('pr-a', i)) FROM generate_series(1, 400) i)) AS freed;
VACUUM pg_history.chunk_index;
SELECT free_pages > 10 AS pages_freed FROM test_chunk_log_space('pg_history.chunks');
SELECT pg_relation_size('pg_history.chunks') AS size0 \gset
-- a pack of chunks fitting pages, as many bytes as were freed: into the
-- pages freed, the log no bigger
\c
SET chunk_free_in_background = off;
SELECT test_cs_put_many('pages', ARRAY(SELECT pr_h(pr_blob('pr-b', i)) FROM generate_series(1, 300) i ORDER BY i),
	ARRAY(SELECT pr_blob('pr-b', i) FROM generate_series(1, 300) i ORDER BY i)) AS stored;
SELECT pg_relation_size('pg_history.chunks') = :size0 AS no_bigger;
SELECT count(*) FILTER (WHERE test_cs_get('pages', pr_h(pr_blob('pr-b', i))) = pr_blob('pr-b', i))
	AS read_back FROM generate_series(1, 300) i;
SELECT ok FROM test_chunk_check('pg_history.chunks', 'pg_history.chunk_index');
-- let go of
SELECT test_cs_free('pages', ARRAY(SELECT pr_h(pr_blob('pr-b', i)) FROM generate_series(1, 300) i)) AS freed;
SELECT test_cs_unref_root('pages', pr_h(pr_blob('pr-keep', 1)));
SELECT test_cs_free('pages', ARRAY[pr_h(pr_blob('pr-keep', 1))]) AS freed;
DROP FUNCTION pr_h(bytea), pr_blob(text, int);
