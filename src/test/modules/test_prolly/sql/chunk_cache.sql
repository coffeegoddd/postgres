-- The backend's cache of decoded chunks (prolly_chunk_cache_size): a chunk
-- read from the store's files twice is read from the cache after, by any
-- read in the session (one read once, as a fold of many rows reads leaves,
-- isn't kept); with the cache off, from the files each time.  A chunk is
-- named by its content, so one cached is never stale.

-- A database of its own, so nothing else counts in it.
SELECT current_database() AS home \gset
CREATE DATABASE chunk_cache;
\c chunk_cache
CREATE SCHEMA cc VERSIONED;
CREATE TABLE cc.t (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
INSERT INTO cc.t SELECT g, repeat('v', 50) || g FROM generate_series(1, 5000) g;
SELECT length(dolt_commit('-Am', 'rows')::text) > 0 AS committed;
UPDATE cc.t SET v = 'changed' WHERE id % 500 = 0;
SELECT length(dolt_commit('-am', 'ten')::text) > 0 AS committed;
\c chunk_cache

-- since(counter): pg_stat_prolly's counter's growth since it was last
-- asked of, this session's flushed first.
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION pg_temp.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;
SELECT pg_stat_force_next_flush();
SELECT pg_temp.since('chunk_cache_hits') >= 0 AS counting,
	pg_temp.since('chunk_file_reads') >= 0 AS reading;

-- The first diff reads its nodes from the files, and so does the second,
-- which keeps them; the third reads them from the cache.
SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::cc.t);
SELECT pg_stat_force_next_flush();
SELECT pg_temp.since('chunk_cache_hits') IS NOT NULL AS counted,
	pg_temp.since('chunk_file_reads') > 0 AS first_reads_files;
SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::cc.t);
SELECT pg_stat_force_next_flush();
SELECT pg_temp.since('chunk_cache_hits') IS NOT NULL AS counted,
	pg_temp.since('chunk_file_reads') > 0 AS second_reads_files;
SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::cc.t);
SELECT pg_stat_force_next_flush();
SELECT pg_temp.since('chunk_cache_hits') > 0 AS third_hits,
	pg_temp.since('chunk_file_reads') AS third_file_reads;

-- What the cache gives is the chunk: the same rows either way.
SET prolly_chunk_cache_size = 0;
CREATE TEMP TABLE uncached AS
	SELECT (to_row).id, (to_row).v FROM dolt_diff('HEAD~1', 'HEAD', NULL::cc.t);
SELECT pg_stat_force_next_flush();
SELECT pg_temp.since('chunk_cache_hits') AS off_hits,
	pg_temp.since('chunk_file_reads') > 0 AS off_reads_files;
RESET prolly_chunk_cache_size;
SELECT count(*) AS same
FROM uncached u JOIN dolt_diff('HEAD~1', 'HEAD', NULL::cc.t) d
	ON (d.to_row).id = u.id AND (d.to_row).v = u.v;

-- A transaction's own chunks, rolled back, aren't kept: a node made then
-- is read again only if it was written.
BEGIN;
UPDATE cc.t SET v = 'rolled back' WHERE id = 1;
SELECT length(dolt_commit('-am', 'gone')::text) > 0 AS committed;
SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::cc.t);
ROLLBACK;
SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::cc.t);

\c :home
DROP DATABASE chunk_cache;
