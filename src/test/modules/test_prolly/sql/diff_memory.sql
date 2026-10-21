-- dolt_diff and dolt_conflicts free what they decoded for a row before
-- the next: a diff of 20,000 rows holds the memory of one row, not of
-- 20,000.  (What a row decoded to is in the tuplestore, which spills.)
-- A merge, the conflicts it leaves and their resolution hold neither
-- side's changes: what they hold is the same however many rows changed.
--
-- Scheduled last: its commits grow the database's chunk journal by about
-- 10 MB, and a journal past prolly.gc_journal_threshold may be collected
-- under a test that reads it by name.

CREATE SCHEMA dm VERSIONED;

-- The backend's memory, read while a set-returning function's rows are
-- read out: what it allocated as it built them is still held then.
CREATE FUNCTION dm.held() RETURNS bigint VOLATILE LANGUAGE sql
	AS $$SELECT sum(total_bytes)::bigint FROM pg_backend_memory_contexts$$;

-- And sampled as rows are written, by a check on each (every 500th row's):
-- the most held while a merge or a resolution wrote them, in dm.peak.
CREATE FUNCTION dm.sample() RETURNS boolean VOLATILE LANGUAGE plpgsql AS $$
DECLARE
	n int := coalesce(nullif(current_setting('dm.n', true), ''), '0')::int + 1;
BEGIN
	PERFORM set_config('dm.n', n::text, false);
	IF n % 500 = 0 THEN
		PERFORM set_config('dm.peak', greatest(dm.held(),
			coalesce(nullif(current_setting('dm.peak', true), ''), '0')::bigint)::text,
			false);
	END IF;
	RETURN true;
END$$;

CREATE TABLE dm.t (id int PRIMARY KEY, v text CHECK (dm.sample()));
SELECT length(dolt_commit('-Am', 'dm empty')::text) AS committed;
INSERT INTO dm.t SELECT g, repeat('x', 100) FROM generate_series(1, 20000) g;
SELECT length(dolt_commit('-Am', 'dm rows')::text) AS committed;

SET work_mem = '64kB';
-- (the backend's cache of decoded chunks off: it holds up to its setting
-- whatever is read, and what's measured is what reading the rows holds)
SET prolly_chunk_cache_size = 0;

-- (when every row's decoding was kept, this held 8 to 25 MB; freed, under
-- half a MB)
SELECT dm.held() AS before \gset
SELECT dm.held() - :before < 4000000 AS bounded, diff_type
FROM dolt_diff('HEAD~1', 'HEAD', NULL::dm.t) LIMIT 1;
SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::dm.t);

UPDATE dm.t SET v = repeat('y', 100);
SELECT dm.held() AS before \gset
SELECT dm.held() - :before < 4000000 AS bounded, diff_type
FROM dolt_diff('HEAD', 'WORKING', NULL::dm.t) LIMIT 1;
SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::dm.t);

-- Every row in conflict.  Finding them walks theirs' changes, reading
-- ours' row at each key, nothing held for a key past it: the summary, the
-- listing, and resolving them hold what they would for a few.  (Both
-- sides' changes held, about 940 bytes a conflict, they held 20 to 40 MB.)
SELECT length(dolt_commit('-Am', 'dm ours')::text) AS committed;
SELECT dolt_branch('dm_side', 'HEAD~1');
SELECT dolt_checkout('dm_side');
UPDATE dm.t SET v = repeat('z', 100);
SELECT length(dolt_commit('-Am', 'dm theirs')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('dm_side'))[2:4];
SELECT dm.held() AS before \gset
SELECT dm.held() - :before < 4000000 AS bounded, num_conflicts
FROM dolt_conflicts();
SELECT dm.held() - :before < 4000000 AS bounded, our_diff_type, their_diff_type
FROM dolt_conflicts(NULL::dm.t) LIMIT 1;
SELECT count(*) FROM dolt_conflicts(NULL::dm.t);
SELECT set_config('dm.peak', '0', false) AS peak, dm.held() AS before \gset
SELECT dolt_conflicts_resolve('--theirs', 'dm.t');
SELECT current_setting('dm.peak')::bigint - :before < 4000000 AS bounded,
	current_setting('dm.peak')::bigint > 0 AS sampled;
SELECT count(*) FROM dm.t WHERE v = repeat('z', 100);
ROLLBACK;

-- A merge writing theirs' changes to 19,999 rows, ours changed another.
SELECT dolt_branch('dm_clean');
SELECT dolt_checkout('dm_clean');
UPDATE dm.t SET v = repeat('w', 100) WHERE id > 1;
SELECT length(dolt_commit('-Am', 'dm clean')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE dm.t SET v = 'ours' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'dm one')::text) AS committed;
BEGIN;
SELECT set_config('dm.peak', '0', false) AS peak, dm.held() AS before \gset
SELECT (dolt_merge('dm_clean'))[2:4];
SELECT current_setting('dm.peak')::bigint - :before < 4000000 AS bounded,
	current_setting('dm.peak')::bigint > 0 AS sampled;
SELECT count(*) FROM dm.t WHERE v = repeat('w', 100);
ROLLBACK;

RESET work_mem;
SELECT dolt_branch('-D', 'dm_side');
SELECT dolt_branch('-D', 'dm_clean');
DROP SCHEMA dm CASCADE;
SELECT length(dolt_commit('-Am', 'dm gone')::text) AS committed;
