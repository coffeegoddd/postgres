-- Folds skipped when nothing changed (#482): reading the working set
-- (dolt_status, dolt_diff from WORKING, dolt_branches' dirty) takes the
-- last fold of the branch while nothing it reads has changed since, and
-- folds again once anything has.  folds_reused counts each one taken.
-- (This suite runs with debug_prolly_verify_fold_caches on: each one taken
-- is folded too, an error if the two differ.)
CREATE DATABASE regress_fold_caches;
\c regress_fold_caches
SET dolt.author = 'Ada Lovelace <ada@example.com>';
CREATE SCHEMA fc VERSIONED;
CREATE TABLE fc.t (id int PRIMARY KEY, v text);
CREATE SEQUENCE fc.s;
CREATE TYPE fc.mood AS ENUM ('sad');
CREATE TABLE fc.m (id int PRIMARY KEY, mood fc.mood);
INSERT INTO fc.t SELECT g, 'v' || g FROM generate_series(1, 100) g;

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
SELECT length(dolt_commit('-Am', 'base')::text) > 0 AS committed;
:flush
SELECT pg_temp.since('folds_reused') >= 0 AS counting;

-- Nothing changed since the commit: its fold is the working set, taken
-- each time, nothing folded (nothing hashed; the check is off to show it).
SET debug_prolly_verify_fold_caches = off;
:flush
SELECT pg_temp.since('bytes_hashed') >= 0 AS counting;
SELECT * FROM dolt_status;
SELECT * FROM dolt_status;
:flush
SELECT pg_temp.since('folds_reused') AS clean_statuses,
	pg_temp.since('bytes_hashed') AS bytes_hashed;
RESET debug_prolly_verify_fold_caches;

-- A row written: folded once, then taken.
UPDATE fc.t SET v = 'changed' WHERE id = 1;
SELECT * FROM dolt_status;
SELECT * FROM dolt_status;
SELECT diff_type, (to_row).v FROM dolt_diff('HEAD', 'WORKING', NULL::fc.t);
:flush
SELECT pg_temp.since('folds_reused') AS after_one_fold;

-- Another branch's dirtiness, without checking it out: each branch's
-- folded once (the checkouts' folds didn't say what they left out), then
-- taken.
SELECT dolt_branch('other');
SELECT dolt_checkout('other');
SELECT dolt_checkout('main');
SELECT name, dirty FROM dolt_branches ORDER BY name;
SELECT name, dirty FROM dolt_branches ORDER BY name;
:flush
SELECT pg_temp.since('folds_reused') AS dirty_taken;

-- Each change below is seen, none taken from the fold before it.
SELECT length(dolt_commit('-am', 'one row')::text) > 0 AS committed;
SELECT * FROM dolt_status;
:flush
SELECT pg_temp.since('folds_reused') AS committed_clean;

-- DDL
ALTER TABLE fc.t ADD COLUMN n int;
SELECT * FROM dolt_status;
:flush
SELECT pg_temp.since('folds_reused') AS after_alter;
SELECT length(dolt_commit('-am', 'altered')::text) > 0 AS committed;

-- an enum's label added
ALTER TYPE fc.mood ADD VALUE 'happy';
SELECT * FROM dolt_status;
:flush
SELECT pg_temp.since('folds_reused') AS after_add_value;
SELECT length(dolt_commit('-am', 'label')::text) > 0 AS committed;

-- a sequence's value: nextval and setval
SELECT nextval('fc.s');
SELECT * FROM dolt_status;
:flush
SELECT pg_temp.since('folds_reused') AS after_nextval;
SELECT length(dolt_commit('-am', 'nextval')::text) > 0 AS committed;
SELECT setval('fc.s', 10);
SELECT * FROM dolt_status;
:flush
SELECT pg_temp.since('folds_reused') AS after_setval;
SELECT length(dolt_commit('-am', 'setval')::text) > 0 AS committed;

-- TRUNCATE
TRUNCATE fc.m;
INSERT INTO fc.m VALUES (1, 'happy');
SELECT length(dolt_commit('-am', 'a mood')::text) > 0 AS committed;
TRUNCATE fc.m;
SELECT * FROM dolt_status;
:flush
SELECT pg_temp.since('folds_reused') AS after_truncate;
SELECT length(dolt_commit('-am', 'truncated')::text) > 0 AS committed;

-- a branch switched to: its own working set (none of its statuses taken;
-- main's, back on it, is: leaving a branch reads nothing of it)
SELECT dolt_checkout('other');
SELECT * FROM dolt_status;
INSERT INTO fc.t VALUES (1000, 'on other');
SELECT * FROM dolt_status;
SELECT dolt_checkout('main');
SELECT * FROM dolt_status;
:flush
SELECT pg_temp.since('folds_reused') AS after_switch;

-- the transaction's own writes and DDL: seen at once, taken from no fold
-- (one taken: the first status, before the transaction wrote)
BEGIN;
SELECT * FROM dolt_status;
INSERT INTO fc.t VALUES (2000, 'in a transaction');
SELECT * FROM dolt_status;
ROLLBACK;
BEGIN;
SELECT * FROM dolt_status;
CREATE TABLE fc.made (id int PRIMARY KEY);
SELECT * FROM dolt_status;
ROLLBACK;
SELECT * FROM dolt_status;
:flush
SELECT pg_temp.since('folds_reused') AS own_writes;

-- a subtransaction's write rolled back: gone again
BEGIN;
SAVEPOINT s;
DELETE FROM fc.t WHERE id = 2;
SELECT * FROM dolt_status;
ROLLBACK TO s;
SELECT * FROM dolt_status;
COMMIT;
SELECT * FROM dolt_status;
:flush
SELECT pg_temp.since('folds_reused') AS after_savepoint;

-- a role a policy names, renamed (a catalog outside the branch)
CREATE ROLE regress_fold_caches_r;
ALTER TABLE fc.t ENABLE ROW LEVEL SECURITY;
CREATE POLICY p ON fc.t TO regress_fold_caches_r USING (true);
SELECT length(dolt_commit('-am', 'policy')::text) > 0 AS committed;
ALTER ROLE regress_fold_caches_r RENAME TO regress_fold_caches_s;
SELECT * FROM dolt_status;
:flush
SELECT pg_temp.since('folds_reused') AS after_role_rename;
DROP POLICY p ON fc.t;
ALTER TABLE fc.t DISABLE ROW LEVEL SECURITY;
DROP ROLE regress_fold_caches_s;
SELECT length(dolt_commit('-am', 'no policy')::text) > 0 AS committed;

-- a collection: the fold's tree is no ref's, so it isn't taken after one
SELECT dolt_gc();
SELECT * FROM dolt_status;
:flush
SELECT pg_temp.since('folds_reused') AS after_gc;

-- forced to read every row: nothing taken
SET debug_prolly_force_full_fold = on;
SELECT * FROM dolt_status;
SELECT * FROM dolt_status;
RESET debug_prolly_force_full_fold;
:flush
SELECT pg_temp.since('folds_reused') AS forced_full;

-- No fold around moves: a hard reset takes the working set known, and a
-- checkout reads none.  Leaving a clean branch, and back: nothing taken,
-- nothing folded.
SELECT length(dolt_commit('-am', 'on other')::text) > 0 AS committed
FROM (SELECT dolt_checkout('other')) c;
SELECT dolt_checkout('main');
SELECT * FROM dolt_status;
SET debug_prolly_verify_fold_caches = off;
:flush
SELECT pg_temp.since('folds_reused') IS NOT NULL AS counting,
	pg_temp.since('bytes_hashed') IS NOT NULL AS hashing;
SELECT dolt_checkout('other');
SELECT dolt_checkout('main');
:flush
SELECT pg_temp.since('folds_reused') AS clean_checkouts,
	pg_temp.since('bytes_hashed') AS bytes_hashed;

RESET debug_prolly_verify_fold_caches;

-- A hard reset of a clean branch: the working set is the head's already.
SELECT dolt_reset('--hard');
:flush
SELECT pg_temp.since('folds_reused') AS clean_reset;

-- A hard reset after a write: folded, and the row put back.
UPDATE fc.t SET v = 'to undo' WHERE id = 3;
SELECT dolt_reset('--hard');
SELECT v FROM fc.t WHERE id = 3;
SELECT * FROM dolt_status;
:flush
SELECT pg_temp.since('folds_reused') AS reset_after_write;

-- A fast-forward: the working set before it taken (by the merge, and by
-- the move), then the move's fold.
SELECT dolt_checkout('other');
UPDATE fc.t SET v = 'forward' WHERE id = 4;
SELECT length(dolt_commit('-am', 'forward')::text) > 0 AS committed;
SELECT dolt_checkout('main');
:flush
SELECT pg_temp.since('folds_reused') IS NOT NULL AS counting;
SELECT (dolt_merge('other'))[2] AS fast_forward;
SELECT v FROM fc.t WHERE id = 4;
SELECT * FROM dolt_status;
:flush
SELECT pg_temp.since('folds_reused') AS fast_forward_taken;

-- Leaving a branch with changes: no checkpoint (the change log keeps
-- nothing a fold frees), nothing read or folded; they stay the branch's,
-- for its next fold.
UPDATE fc.t SET v = 'left' WHERE id = 5;
SELECT * FROM dolt_status;
SET debug_prolly_verify_fold_caches = off;
:flush
SELECT pg_temp.since('fold_rows_read') IS NOT NULL AS counting,
	pg_temp.since('bytes_hashed') IS NOT NULL AS hashing,
	pg_temp.since('chunks_appended') IS NOT NULL AS appending;
SELECT dolt_checkout('other');
SELECT dolt_checkout('main');
:flush
SELECT pg_temp.since('fold_rows_read') AS dirty_leave_rows,
	pg_temp.since('bytes_hashed') AS dirty_leave_hashed,
	pg_temp.since('chunks_appended') AS dirty_leave_chunks;
RESET debug_prolly_verify_fold_caches;
SELECT * FROM dolt_status;
SELECT v FROM fc.t WHERE id = 5;
UPDATE fc.t SET v = 'left again' WHERE id = 6;
BEGIN;
SELECT dolt_checkout('other');
SELECT dolt_checkout('main');
COMMIT;
SELECT * FROM dolt_status;
SELECT v FROM fc.t WHERE id = 6;

-- A diff of two commits reads the working set's definitions, not its rows:
-- nothing of its changes folded (none read), as the diff from WORKING
-- does.
SELECT length(dolt_commit('-am', 'left')::text) > 0 AS committed;
UPDATE fc.t SET v = 'dirty' WHERE id IN (7, 8);
:flush
SELECT pg_temp.since('fold_rows_read') IS NOT NULL AS counting;
SELECT diff_type, (from_row).id, (to_row).v
FROM dolt_diff('HEAD~1', 'HEAD', NULL::fc.t);
:flush
SELECT pg_temp.since('fold_rows_read') AS history_diff_rows;

-- A diff into WORKING folds nothing either: its rows are read from the
-- change log's entries against the baseline, the changed rows alone, no
-- tree made (no chunk written); one from another commit merges them with
-- the tree diff from it to the baseline.  (The check, on, diffs a fold
-- too and compares.)
SET debug_prolly_verify_fold_caches = off;
:flush
SELECT pg_temp.since('chunks_appended') IS NOT NULL AS appending;
SELECT diff_type, (from_row).id, (to_row).v
FROM dolt_diff('HEAD', 'WORKING', NULL::fc.t) ORDER BY 2;
:flush
SELECT pg_temp.since('fold_rows_read') AS working_diff_rows,
	pg_temp.since('chunks_appended') AS working_diff_chunks;
RESET debug_prolly_verify_fold_caches;
SELECT diff_type, (from_row).id, (from_row).v, (to_row).v
FROM dolt_diff('HEAD~1', 'WORKING', NULL::fc.t) ORDER BY 2;
SELECT diff_type, (from_row).id, (to_row).v
FROM dolt_diff('HEAD', 'WORKING', NULL::fc.t) ORDER BY 2;
UPDATE fc.t SET v = 'left' WHERE id = 7;
DELETE FROM fc.t WHERE id = 6;
INSERT INTO fc.t VALUES (2000, 'new');
SELECT diff_type, (from_row).id, (from_row).v, (to_row).id, (to_row).v
FROM dolt_diff('HEAD~1', 'WORKING', NULL::fc.t) ORDER BY coalesce((from_row).id, (to_row).id);

-- So does a status: a table defined as at the head, whose baseline holds
-- the head's rows, changed if its change log has a change since (the first
-- found is enough), no tree made.  (The check, on, folds too and compares.)
SET debug_prolly_verify_fold_caches = off;
:flush
SELECT pg_temp.since('chunks_appended') IS NOT NULL AS appending,
	pg_temp.since('fold_rows_written') IS NOT NULL AS writing;
SELECT * FROM dolt_status;
:flush
SELECT pg_temp.since('chunks_appended') AS status_chunks,
	pg_temp.since('fold_rows_written') AS status_rows_folded;
RESET debug_prolly_verify_fold_caches;
SELECT * FROM dolt_status;
SELECT dolt_reset('--hard');
UPDATE fc.t SET v = v WHERE id = 9;
SELECT * FROM dolt_status;
-- ... and, no table changed, what that status read is the working set,
-- published: the next one takes it as it is, folding and writing nothing.
SET debug_prolly_verify_fold_caches = off;
:flush
SELECT pg_temp.since('folds_reused') IS NOT NULL AS reusing,
	pg_temp.since('chunks_appended') IS NOT NULL AS appending,
	pg_temp.since('fold_rows_written') IS NOT NULL AS writing;
SELECT * FROM dolt_status;
:flush
SELECT pg_temp.since('folds_reused') AS clean_status_reused,
	pg_temp.since('chunks_appended') AS clean_status_chunks,
	pg_temp.since('fold_rows_written') AS clean_status_rows;
RESET debug_prolly_verify_fold_caches;
ALTER TABLE fc.t ADD COLUMN w int;
SELECT * FROM dolt_status;
SELECT dolt_reset('--hard');

-- Changes that cancel out send no row: a row the working set puts back as
-- the first version has it, and one a commit since added that the working
-- set deletes.  (The commit is taken back after.)
INSERT INTO fc.t VALUES (3000, 'added');
SELECT length(dolt_commit('-am', 'added')::text) > 0 AS committed;
UPDATE fc.t SET v = 'v5' WHERE id = 5;
DELETE FROM fc.t WHERE id = 3000;
SELECT diff_type, (from_row).id, (from_row).v, (to_row).id, (to_row).v
FROM dolt_diff('HEAD~2', 'WORKING', NULL::fc.t) ORDER BY coalesce((from_row).id, (to_row).id);
SELECT dolt_reset('--hard', 'HEAD~1');

-- ... and with a definition changed since, not committed: the commits'
-- rows read as the table is defined now (w, which neither had, null),
-- still no row of the working set folded.
ALTER TABLE fc.t ADD COLUMN w int;
UPDATE fc.t SET v = 'dirty', w = 1 WHERE id = 7;
:flush
SELECT pg_temp.since('fold_rows_read') IS NOT NULL AS counting;
SELECT diff_type, (from_row).id, (to_row).v, (to_row).w
FROM dolt_diff('HEAD~1', 'HEAD', NULL::fc.t);
:flush
SELECT pg_temp.since('fold_rows_read') AS redefined_history_diff_rows;
SELECT dolt_reset('--hard');

\c contrib_regression
DROP DATABASE regress_fold_caches;
