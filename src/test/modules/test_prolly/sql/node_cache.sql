-- The shared cache of decoded chunks (prolly_node_cache_size): a chunk one
-- backend read from the store's files is read from the cache by every
-- backend after, so a new session's reads of trees another session read
-- touch no file.  A chunk is named by its content, so one cached is never
-- stale; what a collection removes is never given from it, in any session.

-- A database of its own, so nothing else counts in it.
SELECT current_database() AS home \gset
CREATE DATABASE node_cache;
\c node_cache
CREATE SCHEMA cc VERSIONED;
CREATE TABLE cc.t (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
INSERT INTO cc.t SELECT g, repeat('v', 50) || g FROM generate_series(1, 5000) g;
SELECT length(dolt_commit('-Am', 'rows')::text) > 0 AS committed;
UPDATE cc.t SET v = 'changed' WHERE id % 500 = 0;
SELECT length(dolt_commit('-am', 'ten')::text) > 0 AS committed;
\c node_cache

-- since(counter): pg_stat_prolly's counter's growth since it was last
-- asked of, this session's flushed first.  (A new session asks afresh.)
SET check_function_bodies = off;
CREATE FUNCTION public.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;
RESET check_function_bodies;
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
SELECT pg_stat_force_next_flush();
SELECT since('node_cache_hits') >= 0 AS counting,
	since('node_cache_misses') >= 0 AS counting_misses,
	since('chunk_file_reads') >= 0 AS reading;

-- This session's diff reads its nodes, from the files or (the commit's
-- own, let in as it committed) from the cache.
CREATE TEMP TABLE first_read AS
	SELECT (to_row).id, (to_row).v FROM dolt_diff('HEAD~1', 'HEAD', NULL::cc.t);
SELECT pg_stat_force_next_flush();
SELECT since('node_cache_hits') + since('chunk_file_reads') > 0 AS read;
SELECT count(*) AS rows FROM first_read;

-- A new session, whose own memory is empty, reads them all from the cache:
-- every node a hit, none from the files.
\c node_cache
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
SELECT pg_stat_force_next_flush();
SELECT since('node_cache_hits') >= 0 AS counting,
	since('chunk_file_reads') >= 0 AS reading;
CREATE TEMP TABLE second_read AS
	SELECT (to_row).id, (to_row).v FROM dolt_diff('HEAD~1', 'HEAD', NULL::cc.t);
SELECT pg_stat_force_next_flush();
SELECT since('node_cache_hits') > 0 AS new_session_hits,
	since('chunk_file_reads') AS new_session_file_reads;
-- What the cache gives is the chunk: the same rows.
SELECT count(*) AS rows,
	count(*) FILTER (WHERE v = 'changed') AS changed,
	min(id) AS first, max(id) AS last
FROM second_read;
SELECT count(*) AS same FROM second_read s JOIN dolt_diff('HEAD~1', 'HEAD', NULL::cc.t) d
	ON (d.to_row).id = s.id AND (d.to_row).v = s.v;

\c :home
DROP DATABASE node_cache;

-- A chunk a collection removes isn't given from the cache after: not in
-- the session that cached it, nor the one that collected, nor a new one.
-- Each finds nothing, as the store says.  The same content put again has
-- the same hash and is read again.
CREATE DATABASE node_cache_gc;
\c node_cache_gc
CREATE EXTENSION test_prolly;
SET check_function_bodies = off;
CREATE FUNCTION public.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;
RESET check_function_bodies;
SELECT test_chunk_put('cc kept') AS kept \gset
SELECT test_chunk_put('cc dropped') AS dropped \gset
SELECT test_chunk_put_node(1, ARRAY[:'kept']) AS node \gset
SELECT test_chunk_move_root(NULL, :'node');
-- (cached: by this session's commit, or its first read; its second is a hit)
\c node_cache_gc
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
SELECT pg_stat_force_next_flush();
SELECT since('node_cache_hits') >= 0 AS counting;
SELECT test_chunk_get(:'dropped') IS NOT NULL AS read;
SELECT test_chunk_get(:'dropped') IS NOT NULL AS read_again;
SELECT pg_stat_force_next_flush();
SELECT since('node_cache_hits') > 0 AS read_from_cache;
\c node_cache_gc
SELECT kept, freed FROM test_chunk_gc(false);
SELECT test_chunk_has(:'dropped') AS has_dropped,
	test_chunk_get(:'dropped') IS NULL AS get_none;
\c node_cache_gc
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
SELECT pg_stat_force_next_flush();
SELECT since('node_cache_misses') >= 0 AS counting,
	since('chunk_file_reads') >= 0 AS reading;
SELECT test_chunk_has(:'dropped') AS has_dropped,
	test_chunk_get(:'dropped') IS NULL AS get_none;
-- The kept chunk was cached before the collection, so after it a read
-- misses and reads the file.  A chunk read from the files is kept once it
-- is read again (one read once isn't), so the third read is a hit.
SELECT convert_from(test_chunk_get(:'kept'), 'UTF8') AS kept;
SELECT pg_stat_force_next_flush();
SELECT since('node_cache_misses') > 0 AS missed,
	since('chunk_file_reads') > 0 AS read_file;
SELECT convert_from(test_chunk_get(:'kept'), 'UTF8') AS kept;
SELECT pg_stat_force_next_flush();
SELECT since('node_cache_misses') > 0 AS missed_again,
	since('chunk_file_reads') > 0 AS read_file_again;
SELECT convert_from(test_chunk_get(:'kept'), 'UTF8') AS kept;
SELECT pg_stat_force_next_flush();
SELECT since('node_cache_misses') AS misses_after,
	since('chunk_file_reads') AS file_reads_after;
-- The same content again: the same hash, readable again.
SELECT test_chunk_put('cc dropped') = :'dropped' AS same_hash;
\c node_cache_gc
SELECT convert_from(test_chunk_get(:'dropped'), 'UTF8') AS dropped_again;

\c :home
DROP DATABASE node_cache_gc;
