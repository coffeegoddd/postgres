-- A move from a dirty working set folds it only to compare it with the
-- target, and to have a baseline to advance to the target after: that
-- fold's trees are kept in the backend's memory, not put in the chunk
-- store, and only the baseline as advanced (its few nodes) is put.  So a
-- hard reset of 1,000 dirty rows over 100,000 appends a handful of chunks,
-- not the fold's thousand-odd, and what is put names only what the store
-- has: the next commit's fold builds on it.
CREATE DATABASE regress_compared_folds;
\c regress_compared_folds
SET dolt.author = 'Ada Lovelace <ada@example.com>';
CREATE EXTENSION test_prolly;
CREATE SCHEMA cf VERSIONED;
CREATE TABLE cf.t (id int PRIMARY KEY, v text);
INSERT INTO cf.t SELECT g, 'value ' || g FROM generate_series(1, 100000) g;
SELECT length(dolt_commit('-Am', 'base')::text) > 0 AS committed;
UPDATE cf.t SET v = 'second' WHERE id = 1;
SELECT length(dolt_commit('-am', 'second')::text) > 0 AS committed;

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

-- (rows written, not pages given back: kept_pages tests those; and the
-- check, which folds again, off to count)
SET debug_prolly_page_roots = off;
SET debug_prolly_verify_fold_caches = off;

-- 1,000 dirty rows reset: folded once (1,000 candidates), almost nothing
-- appended.
UPDATE cf.t SET v = 'dirty' WHERE id % 100 = 0;
:flush
SELECT pg_temp.since('chunks_appended') IS NOT NULL AS appending,
	pg_temp.since('fold_candidates') IS NOT NULL AS counting;
SELECT dolt_reset('--hard');
:flush
SELECT pg_temp.since('fold_candidates') AS reset_candidates,
	pg_temp.since('chunks_appended') < 20 AS reset_appends_few;
SELECT count(*) FILTER (WHERE v = 'dirty') AS dirty_left FROM cf.t;
SELECT * FROM dolt_status;
RESET debug_prolly_verify_fold_caches;
SELECT test_fold_root(true) = prolly_root_hash('main') AS head_rows;

-- A reset to an older commit from a dirty working set: the same.  (Rows
-- written as never before: a fold of the same rows would name chunks the
-- store has from before.)
SET debug_prolly_verify_fold_caches = off;
UPDATE cf.t SET v = 'dirty again' WHERE id % 100 = 0;
:flush
SELECT pg_temp.since('chunks_appended') IS NOT NULL AS appending,
	pg_temp.since('fold_candidates') IS NOT NULL AS counting;
SELECT dolt_reset('--hard', 'HEAD~1');
:flush
SELECT pg_temp.since('fold_candidates') AS older_reset_candidates,
	pg_temp.since('chunks_appended') < 20 AS older_reset_appends_few;
SELECT v FROM cf.t WHERE id = 1;
RESET debug_prolly_verify_fold_caches;
SELECT test_fold_root(true) = prolly_root_hash('main') AS older_rows;

-- The baseline put builds the next commit, in the reset's transaction and
-- in the next.
BEGIN;
UPDATE cf.t SET v = 'dirty' WHERE id % 100 = 0;
SELECT dolt_reset('--hard');
UPDATE cf.t SET v = 'kept' WHERE id = 7;
SELECT length(dolt_commit('-am', 'in the reset''s transaction')::text) > 0
	AS committed;
COMMIT;
SELECT diff_type, (to_row).id, (to_row).v
FROM dolt_diff('HEAD~1', 'HEAD', NULL::cf.t);
SELECT test_fold_root(true) = prolly_root_hash('main') AS committed_rows;
UPDATE cf.t SET v = 'dirty' WHERE id % 100 = 0;
SELECT dolt_reset('--hard');
UPDATE cf.t SET v = 'after' WHERE id = 8;
SELECT length(dolt_commit('-am', 'after the reset')::text) > 0 AS committed;
SELECT diff_type, (to_row).id, (to_row).v
FROM dolt_diff('HEAD~1', 'HEAD', NULL::cf.t);
SELECT test_fold_root(true) = prolly_root_hash('main') AS committed_after;

-- A reset rolled back to a savepoint leaves nothing of it behind.
BEGIN;
UPDATE cf.t SET v = 'dirty' WHERE id % 100 = 0;
SAVEPOINT s;
SELECT dolt_reset('--hard');
ROLLBACK TO SAVEPOINT s;
SELECT count(*) FILTER (WHERE v = 'dirty') AS dirty_kept FROM cf.t;
SELECT dolt_reset('--hard');
COMMIT;
SELECT count(*) FILTER (WHERE v = 'dirty') AS dirty_left FROM cf.t;
SELECT test_fold_root(true) = prolly_root_hash('main') AS rolled_back_rows;

-- A move whose baseline can't be advanced (the definitions moved too)
-- persists the fold it compared with, and folds again from it.
ALTER TABLE cf.t ADD COLUMN w int;
UPDATE cf.t SET v = 'dirty' WHERE id % 100 = 0;
SELECT dolt_reset('--hard');
SELECT count(*) FILTER (WHERE v = 'dirty') AS dirty_left FROM cf.t;
SELECT * FROM dolt_status;
UPDATE cf.t SET v = 'after' WHERE id = 9;
SELECT length(dolt_commit('-am', 'after a redefining reset')::text) > 0
	AS committed;
SELECT test_fold_root(true) = prolly_root_hash('main') AS redefined_rows;

-- Long values stored out of line, by content: a tree kept in memory takes
-- a stored value's tree by its pointer, as any fold does, reading and
-- hashing none of its bytes (20 new values of 200 kB: 4 MB if each were
-- read and hashed again).
CREATE TABLE cf.big (id int PRIMARY KEY, big text STORAGE EXTERNAL);
INSERT INTO cf.big SELECT g, 'small' FROM generate_series(1, 50) g;
SELECT length(dolt_commit('-Am', 'big')::text) > 0 AS committed;
SET debug_prolly_verify_fold_caches = off;
UPDATE cf.big SET big = (SELECT string_agg(md5(id || ':' || i), '')
						 FROM generate_series(1, 6250) i)
WHERE id <= 20;
:flush
SELECT pg_temp.since('bytes_hashed') IS NOT NULL AS hashing,
	pg_temp.since('chunks_appended') IS NOT NULL AS appending;
SELECT dolt_reset('--hard');
:flush
SELECT pg_temp.since('bytes_hashed') < 1000000 AS values_not_hashed,
	pg_temp.since('chunks_appended') < 20 AS long_reset_appends_few;
RESET debug_prolly_verify_fold_caches;
SELECT count(*) FILTER (WHERE big <> 'small') AS long_left FROM cf.big;
SELECT test_fold_root(true) = prolly_root_hash('main') AS long_rows;

-- The same, not advanced (the definitions moved too): the fold's trees
-- persisted, the values' named as stored.
ALTER TABLE cf.big ADD COLUMN w int;
UPDATE cf.big SET big = (SELECT string_agg(md5(id || '/' || i), '')
						 FROM generate_series(1, 6250) i)
WHERE id <= 20;
SELECT dolt_reset('--hard');
SELECT count(*) FILTER (WHERE big <> 'small') AS long_left FROM cf.big;
SELECT test_fold_root(true) = prolly_root_hash('main') AS long_redefined_rows;

-- Past maintenance_work_mem kept, the step puts in the store as before:
-- the fold's trees appended, the rows put back all the same.
SET maintenance_work_mem = '1MB';
SET debug_prolly_verify_fold_caches = off;
UPDATE cf.t SET v = 'spilled ' || id WHERE id % 100 = 0;
:flush
SELECT pg_temp.since('chunks_appended') IS NOT NULL AS appending;
SELECT dolt_reset('--hard');
:flush
SELECT pg_temp.since('chunks_appended') > 500 AS spilled_appends;
RESET debug_prolly_verify_fold_caches;
RESET maintenance_work_mem;
SELECT count(*) FILTER (WHERE v LIKE 'spilled%') AS spilled_left FROM cf.t;
SELECT * FROM dolt_status;
SELECT test_fold_root(true) = prolly_root_hash('main') AS spilled_rows;

-- Everything the refs name is in the store, the baselines' trees with it,
-- and a full collection keeps what they reach.
SELECT test_refs_walk_count(NULL) > 0 AS walked;
SELECT kept > 0 AS kept FROM test_chunk_gc(true);
SELECT test_refs_walk_count(NULL) > 0 AS walked_after_gc;
UPDATE cf.t SET v = 'after gc' WHERE id = 10;
SELECT length(dolt_commit('-am', 'after the collection')::text) > 0
	AS committed;
SELECT test_fold_root(true) = prolly_root_hash('main') AS collected_rows;

\c contrib_regression
DROP DATABASE regress_compared_folds;
