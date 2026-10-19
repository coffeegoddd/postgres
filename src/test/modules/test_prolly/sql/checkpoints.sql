-- Working-set checkpoints: a branch's working set folded as a commit
-- would fold it, without a commit, so later folds start from it.  One is
-- taken when asked for, and when a session leaves a branch with changes.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
-- (Folded by the change finder "pages": what it reads, and the map epochs
-- its folds raise, show when a checkpoint is taken.  "log"'s checkpoints
-- are change_log's.)
SET debug_prolly_change_finder = pages;

CREATE FUNCTION pg_temp.fold(OUT pages int8, OUT full_tables int8,
	OUT same_as_full bool) AS $$
DECLARE
	inc text;
BEGIN
	PERFORM test_fold_counters(true);
	inc := test_fold_root();
	SELECT c.pages, c.full_tables INTO pages, full_tables
	  FROM test_fold_counters(true) c;
	same_as_full := inc = test_fold_root(true);
END $$ LANGUAGE plpgsql;

CREATE SCHEMA cp VERSIONED;
CREATE TABLE cp.t (id int PRIMARY KEY, v text);
INSERT INTO cp.t SELECT g, 'v' || g FROM generate_series(1, 20000) g;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;

-- Changes, then a checkpoint: later folds read only what changed since.
UPDATE cp.t SET v = 'one' WHERE id = 1;
UPDATE cp.t SET v = 'two' WHERE id = 10000;
SELECT pages > 0 AS reads_changes FROM pg_temp.fold();
SELECT prolly_checkpoint() = test_fold_root(true) AS checkpoint_is_working_set;
SELECT * FROM pg_temp.fold();

-- The changes are still the working set's, against its head.
SELECT table_name, status FROM dolt_status;
SELECT diff_type, (to_row).id FROM dolt_diff('HEAD', 'WORKING', NULL::cp.t) ORDER BY 2;

-- More changes after it, then a commit of them all.
UPDATE cp.t SET v = 'three' WHERE id = 20000;
SELECT pages <= 3 AS reads_the_new_change, full_tables, same_as_full FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'three changes')::text) AS committed;
SELECT diff_type, (to_row).id FROM dolt_diff('HEAD~1', 'HEAD', NULL::cp.t) ORDER BY 2;
SELECT count(*) AS clean FROM dolt_status;
SELECT * FROM pg_temp.fold();

-- A checkpoint rolled back is gone.
UPDATE cp.t SET v = 'four' WHERE id = 4;
BEGIN;
SELECT length(prolly_checkpoint()) AS checkpointed;
ROLLBACK;
SELECT pages > 0 AS reads_since_the_commit, same_as_full FROM pg_temp.fold();

-- Leaving a branch with changes takes a checkpoint of it.  (A checkpoint
-- begins the table's fold anew, raising its map's epoch.)
SELECT pg_relation_filenode('cp.t') AS t \gset
SELECT dolt_branch('other');
SELECT test_bmap_epoch(:t) AS e0 \gset
SELECT dolt_checkout('other');
SELECT test_bmap_epoch(:t) > :e0 AS checkpointed;
SELECT dolt_checkout('main');
SELECT * FROM pg_temp.fold();
SELECT diff_type, (to_row).id FROM dolt_diff('HEAD', 'WORKING', NULL::cp.t) ORDER BY 2;

-- Leaving it again, nothing changed since: none.
SELECT test_bmap_epoch(:t) AS e1 \gset
SELECT dolt_checkout('other');
SELECT test_bmap_epoch(:t) = :e1 AS not_again;
SELECT dolt_checkout('main');

-- Nor in a transaction block, which may be read only.
UPDATE cp.t SET v = 'five' WHERE id = 5;
BEGIN READ ONLY;
SELECT dolt_checkout('other');
SELECT dolt_checkout('main');
COMMIT;
SELECT test_bmap_epoch(:t) = :e1 AS none_in_a_block;

-- Leaving one without changes takes none.
SELECT length(dolt_commit('-am', 'four and five')::text) AS committed;
SELECT test_bmap_epoch(:t) AS e2 \gset
SELECT dolt_checkout('other');
SELECT test_bmap_epoch(:t) = :e2 AS clean_takes_none;
SELECT dolt_checkout('main');

SELECT dolt_branch('-D', 'other');
DROP SCHEMA cp CASCADE;
