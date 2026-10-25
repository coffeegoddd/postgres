-- The commit cache: walking history (HEAD~n, a commit by its hash) takes
-- each commit's links from a table in shared memory, so a walk reads each
-- commit once, not once each time a backend's chunk cache lets go of it.
-- What a transaction read is everyone's only once it commits, and a
-- collection lets go of everything.

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
CREATE FUNCTION pg_temp.work() RETURNS TABLE (counter text, grew int8)
LANGUAGE sql AS $$
	SELECT c, pg_temp.since(c)
	FROM unnest(ARRAY['commits_read', 'commit_cache_hits',
					  'commit_cache_misses']) c
$$;

-- resolves(ref): whether dolt_hashof() finds ref
CREATE FUNCTION pg_temp.resolves(ref text) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	PERFORM dolt_hashof(ref);
	RETURN true;
EXCEPTION WHEN undefined_object THEN
	RETURN false;
END
$$;

SHOW prolly_commit_cache_entries;

CREATE SCHEMA ccache VERSIONED;
CREATE TABLE ccache.t (id int PRIMARY KEY, v int);
INSERT INTO ccache.t VALUES (1, 0);
SELECT (dolt_commit('-Am', 'cc0'))[1] AS base \gset
UPDATE ccache.t SET v = 1;
SELECT length(dolt_commit('-am', 'cc1')::text) > 0 AS committed;
UPDATE ccache.t SET v = 2;
SELECT length(dolt_commit('-am', 'cc2')::text) > 0 AS committed;
UPDATE ccache.t SET v = 3;
SELECT length(dolt_commit('-am', 'cc3')::text) > 0 AS committed;
UPDATE ccache.t SET v = 4;
SELECT length(dolt_commit('-am', 'cc4')::text) > 0 AS committed;
:flush
SELECT count(*) AS counters FROM pg_temp.work();

-- A walk of four steps, its commits new to the cache: each read once.
SELECT dolt_hashof('HEAD~4') = :'base' AS resolved;
:flush
SELECT * FROM pg_temp.work();

-- Walked again (another transaction): none read, each found.
SELECT dolt_hashof('HEAD~4') = :'base' AS resolved;
:flush
SELECT * FROM pg_temp.work();

-- Without the backend's chunk cache, still none read.
SET prolly_chunk_cache_size = 0;
SELECT dolt_hashof('HEAD~4') = :'base' AS resolved;
SELECT dolt_hashof(:'base') = :'base' AS by_hash;
:flush
SELECT * FROM pg_temp.work();
RESET prolly_chunk_cache_size;

-- A commit a transaction made and read is its own: found again inside
-- it, never after it rolls back (the commit is in no store).
BEGIN;
UPDATE ccache.t SET v = 5;
SELECT (dolt_commit('-am', 'cc5'))[1] AS gone \gset
SELECT dolt_hashof('HEAD~1') IS NOT NULL AS walked;
SELECT dolt_hashof(:'gone') = :'gone' AS own;
ROLLBACK;
:flush
SELECT counter, grew > 0 AS grew FROM pg_temp.work()
WHERE counter = 'commit_cache_hits';
SELECT pg_temp.resolves(:'gone') AS gone_resolves;
:flush
SELECT count(*) AS counters FROM pg_temp.work();

-- Nor one a subtransaction made and read, once it rolls back.
BEGIN;
SAVEPOINT s;
UPDATE ccache.t SET v = 6;
SELECT (dolt_commit('-am', 'cc6'))[1] AS sub \gset
SELECT dolt_hashof(:'sub') = :'sub' AS read_in_savepoint;
ROLLBACK TO s;
COMMIT;
SELECT pg_temp.resolves(:'sub') AS sub_resolves;

-- A collection begins the store's next generation: every commit read
-- again, then found.
SELECT length(dolt_gc()::text) > 0 AS collected;
:flush
SELECT count(*) AS counters FROM pg_temp.work();
SELECT dolt_hashof('HEAD~4') = :'base' AS resolved;
:flush
SELECT * FROM pg_temp.work();
SELECT dolt_hashof('HEAD~4') = :'base' AS resolved;
:flush
SELECT * FROM pg_temp.work();

DROP SCHEMA ccache CASCADE;
SELECT length(dolt_commit('-Am', 'drop ccache')::text) > 0 AS committed;
