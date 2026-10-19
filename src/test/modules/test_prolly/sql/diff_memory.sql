-- dolt_diff and dolt_conflicts free what they decoded for a row before
-- the next: a diff of 20,000 rows holds the memory of one row, not of
-- 20,000.  (What a row decoded to is in the tuplestore, which spills.)
--
-- Scheduled last: its commits grow the database's chunk journal by about
-- 7 MB, and a journal past prolly.gc_journal_threshold may be collected
-- under a test that reads it by name.

CREATE SCHEMA dm VERSIONED;
CREATE TABLE dm.t (id int PRIMARY KEY, v text);
SELECT length(dolt_commit('-Am', 'dm empty')::text) AS committed;
INSERT INTO dm.t SELECT g, repeat('x', 100) FROM generate_series(1, 20000) g;
SELECT length(dolt_commit('-Am', 'dm rows')::text) AS committed;

-- The backend's memory, read while a set-returning function's rows are
-- read out: what it allocated as it built them is still held then.
CREATE FUNCTION dm.held() RETURNS bigint VOLATILE LANGUAGE sql
	AS $$SELECT sum(total_bytes)::bigint FROM pg_backend_memory_contexts$$;
SET work_mem = '64kB';

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

-- Every row in conflict.  (Finding them holds both sides' changes, as
-- the merge did: what's held past that, for the rows, is what's bounded.)
SELECT length(dolt_commit('-Am', 'dm ours')::text) AS committed;
SELECT dolt_branch('dm_side', 'HEAD~1');
SELECT dolt_checkout('dm_side');
UPDATE dm.t SET v = repeat('z', 100);
SELECT length(dolt_commit('-Am', 'dm theirs')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('dm_side'))[2:4];
SELECT * FROM dolt_conflicts();
SELECT dm.held() AS found FROM dolt_conflicts() \gset
SELECT dm.held() - :found < 4000000 AS bounded, our_diff_type, their_diff_type
FROM dolt_conflicts(NULL::dm.t) LIMIT 1;
SELECT count(*) FROM dolt_conflicts(NULL::dm.t);
ROLLBACK;

RESET work_mem;
SELECT dolt_branch('-D', 'dm_side');
DROP SCHEMA dm CASCADE;
SELECT length(dolt_commit('-Am', 'dm gone')::text) AS committed;
