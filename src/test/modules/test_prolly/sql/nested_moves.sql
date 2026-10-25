-- A merge, cherry-pick, revert or stash apply moves the branch: nothing
-- that moves a head or the working set (another of them, a reset, a
-- checkout, a commit) runs inside one, as from a check constraint's
-- function the rows it writes call.  It is refused; caught, the outer one
-- goes on.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE SCHEMA nm VERSIONED;
-- (runs nm.q's statement once, from inside whatever writes the row)
CREATE FUNCTION nm.hook(x int) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	q text := coalesce(current_setting('nm.q', true), '');
BEGIN
	IF q <> '' THEN
		PERFORM set_config('nm.q', '', false);
		IF coalesce(current_setting('nm.catch', true), '') <> '' THEN
			BEGIN
				EXECUTE q;
			EXCEPTION WHEN feature_not_supported THEN
				RAISE NOTICE 'caught: %', SQLERRM;
			END;
		ELSE
			EXECUTE q;
		END IF;
	END IF;
	RETURN true;
END $$;
CREATE TABLE nm.ck (id int PRIMARY KEY, x int CHECK (nm.hook(x)));
SELECT length(dolt_commit('-Am', 'ck')::text) AS committed;
SELECT dolt_checkout('-b', 'nm_side') IS NOT NULL AS on_side;
INSERT INTO nm.ck VALUES (1, 1);
SELECT length(dolt_commit('-Am', 'side')::text) AS committed;
SELECT dolt_hashof('HEAD') AS side \gset
SELECT dolt_checkout('-b', 'nm_other') IS NOT NULL AS on_other;
INSERT INTO nm.ck VALUES (3, 3);
SELECT length(dolt_commit('-Am', 'other')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
INSERT INTO nm.ck VALUES (2, 2);
SELECT length(dolt_commit('-Am', 'main')::text) AS committed;
SELECT dolt_hashof('HEAD') AS main_head \gset

-- Inside a merge (its rows written into nm.ck call the hook):
SET nm.q = 'SELECT dolt_merge(''nm_other'', ''-m'', ''inner'')';
SELECT dolt_merge('nm_side', '-m', 'outer');
SET nm.q = 'SELECT dolt_revert(''HEAD'')';
SELECT dolt_merge('nm_side', '-m', 'outer');
SET nm.q = 'SELECT dolt_cherry_pick(''nm_other'')';
SELECT dolt_merge('nm_side', '-m', 'outer');
SET nm.q = 'SELECT dolt_reset(''--hard'', ''HEAD~1'')';
SELECT dolt_merge('nm_side', '-m', 'outer');
SET nm.q = 'SELECT dolt_reset(''--hard'')';
SELECT dolt_merge('nm_side', '-m', 'outer');
SET nm.q = 'SELECT dolt_reset(''--soft'', ''HEAD~1'')';
SELECT dolt_merge('nm_side', '-m', 'outer');
SET nm.q = 'SELECT dolt_checkout(''nm_other'')';
SELECT dolt_merge('nm_side', '-m', 'outer');
SET nm.q = 'SELECT dolt_commit(''--allow-empty'', ''-m'', ''inner'')';
SELECT dolt_merge('nm_side', '-m', 'outer');
-- (a stash applied inside one)
CREATE TABLE nm.stashed (id int PRIMARY KEY);
SELECT dolt_stash('push', 'nm_s') IS NOT NULL AS stashed;
SET nm.q = 'SELECT dolt_stash(''pop'', ''nm_s'')';
SELECT dolt_merge('nm_side', '-m', 'outer');
SELECT dolt_hashof('HEAD') = :'main_head' AS unmoved,
	   (SELECT count(*) FROM nm.ck) AS ck_rows;

-- Inside a cherry-pick (of nm_side's head, its row written):
SET nm.q = 'SELECT dolt_cherry_pick(''nm_other'')';
SELECT dolt_cherry_pick('nm_side');
SELECT dolt_hashof('HEAD') = :'main_head' AS unmoved;

-- Caught, the inner one changes nothing, and the outer goes on.
SET nm.catch = 'on';
SET nm.q = 'SELECT dolt_reset(''--hard'', ''HEAD~1'')';
SELECT (dolt_merge('nm_side', '-m', 'outer'))[2:4] AS merged;
RESET nm.catch;
SELECT id, x FROM nm.ck ORDER BY id;
-- (and later moves run: the count of running ones is back to none)
SELECT length(dolt_cherry_pick('nm_other')::text) > 0 AS picked;
SELECT length(dolt_stash('pop', 'nm_s')::text) > 0 AS popped;
SELECT to_regclass('nm.stashed') IS NOT NULL AS stash_back;

SELECT dolt_reset('--hard') IS NOT NULL AS reset;
SELECT dolt_branch('-D', 'nm_side');
SELECT dolt_branch('-D', 'nm_other');
DROP SCHEMA nm CASCADE;
SELECT length(dolt_commit('-Am', 'nm gone')::text) AS committed;
