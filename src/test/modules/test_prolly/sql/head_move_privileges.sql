-- Moving a branch's head (a commit, a reset of any kind, a merge, a
-- cherry-pick, a revert, a rebase, a branch's deletion, rename or forced
-- move, an integration) takes owning the branch, the database's CREATE
-- privilege, or pg_prolly_write's privileges: a role with only SELECT on
-- the tables can't move a branch every session on it reads (#895).
CREATE DATABASE regress_head_moves;
\c regress_head_moves
CREATE EXTENSION test_prolly;
CREATE SCHEMA hm VERSIONED;
CREATE TABLE hm.t (a int PRIMARY KEY);
CREATE ROLE regress_hm_reader;
CREATE ROLE regress_hm_inserter;
CREATE ROLE regress_hm_writer IN ROLE pg_prolly_write;
CREATE ROLE regress_hm_creator;
CREATE ROLE regress_hm_owner;
GRANT USAGE ON SCHEMA hm TO PUBLIC;
GRANT SELECT ON hm.t TO regress_hm_reader;
GRANT SELECT, INSERT ON hm.t TO regress_hm_inserter;
GRANT ALL ON hm.t TO regress_hm_writer, regress_hm_creator, regress_hm_owner;
GRANT CREATE ON DATABASE regress_head_moves TO regress_hm_creator;
INSERT INTO hm.t VALUES (1);
SELECT length(dolt_commit('-Am', 'one')::text) AS committed;
INSERT INTO hm.t VALUES (2);
SELECT length(dolt_commit('-Am', 'two')::text) AS committed;
SELECT dolt_branch('feat');
SELECT dolt_branch('gone');
SELECT dolt_checkout('feat');
INSERT INTO hm.t VALUES (10);
SELECT length(dolt_commit('-Am', 'feat')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO hm.t VALUES (3);
SELECT length(dolt_commit('-Am', 'three')::text) AS committed;

-- a branch the owner role made, while it could make branches
GRANT CREATE ON DATABASE regress_head_moves TO regress_hm_owner;
SET ROLE regress_hm_owner;
SELECT dolt_branch('mine');
SELECT dolt_branch('mine2');
RESET ROLE;
REVOKE CREATE ON DATABASE regress_head_moves FROM regress_hm_owner;

-- Each statement list run, then undone: 'ok', 'denied' (insufficient
-- privilege), or the error otherwise.
CREATE FUNCTION public.try(VARIADIC q text[]) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	s text;
BEGIN
	BEGIN
		FOREACH s IN ARRAY q LOOP
			EXECUTE s;
		END LOOP;
		RAISE EXCEPTION 'undo';
	EXCEPTION
		WHEN insufficient_privilege THEN
			RETURN 'denied';
		WHEN OTHERS THEN
			IF SQLERRM = 'undo' THEN
				RETURN 'ok';
			END IF;
			RETURN 'error: ' || SQLERRM;
	END;
END $$;

CREATE TABLE public.ops (n int, op text, q text[]);
INSERT INTO public.ops VALUES
	(1, 'commit', ARRAY[$$SELECT dolt_commit('--allow-empty', '-m', 'empty')$$]),
	(2, 'reset --soft', ARRAY[$$SELECT dolt_reset('--soft', 'HEAD~1')$$]),
	(3, 'reset (mixed)', ARRAY[$$SELECT dolt_reset('HEAD~1')$$]),
	(4, 'reset --hard', ARRAY[$$SELECT dolt_reset('--hard', 'HEAD~1')$$]),
	(5, 'reset --hard HEAD', ARRAY[$$SELECT dolt_reset('--hard')$$]),
	(6, 'merge', ARRAY[$$SELECT dolt_merge('feat')$$]),
	(7, 'merge --squash', ARRAY[$$SELECT dolt_merge('--squash', 'feat')$$]),
	(8, 'cherry-pick', ARRAY[$$SELECT dolt_cherry_pick('feat')$$]),
	(9, 'revert', ARRAY[$$SELECT dolt_revert('HEAD')$$]),
	(10, 'rebase', ARRAY[$$SELECT dolt_rebase('feat')$$]),
	(11, 'branch -d', ARRAY[$$SELECT dolt_branch('-D', 'gone')$$]),
	(12, 'branch -m', ARRAY[$$SELECT dolt_branch('-m', 'gone', 'gone2')$$]),
	(13, 'branch -f', ARRAY[$$SELECT dolt_branch('-f', 'gone', 'feat')$$]),
	(14, 'branch -c -f', ARRAY[$$SELECT dolt_branch('-c', '-f', 'feat', 'gone')$$]);
GRANT SELECT ON public.ops TO PUBLIC;
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/head_moves/' || txid_current() AS url \gset
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_push('origin', 'main') IS NOT NULL AS pushed;
SELECT dolt_hashof('main') AS main_head \gset

-- the superuser: every one
SELECT n, op, try(VARIADIC q) FROM public.ops ORDER BY n;

-- SELECT only: none (nor with INSERT too)
SET ROLE regress_hm_reader;
SELECT n, op, try(VARIADIC q) FROM public.ops ORDER BY n;
SET ROLE regress_hm_inserter;
SELECT n, op, try(VARIADIC q) FROM public.ops ORDER BY n;
SELECT dolt_reset('--soft', 'HEAD~1');
SELECT dolt_branch('-D', 'gone');
-- (nor a pull: refused before the fetch)
SELECT dolt_pull('origin', 'main');
-- (a branch not made yet isn't made either)
SELECT dolt_branch('newer');
RESET ROLE;
SELECT dolt_hashof('main') = :'main_head' AS head_kept;

-- pg_prolly_write's privileges, or the database's CREATE: every one
SET ROLE regress_hm_writer;
SELECT n, op, try(VARIADIC q) FROM public.ops ORDER BY n;
SET ROLE regress_hm_creator;
SELECT n, op, try(VARIADIC q) FROM public.ops ORDER BY n;
-- (and an integration of another's branch into main)
SET ROLE regress_hm_writer;
SELECT dolt_checkout('gone');
SELECT try($$SELECT dolt_integrate('-m', 'in')$$) AS integrate;
SELECT dolt_checkout('main');
RESET ROLE;

-- the branch's owner: its own branch's head, not main's
SET ROLE regress_hm_owner;
SELECT n, op, try(VARIADIC q) FROM public.ops WHERE n IN (1, 6, 11, 12) ORDER BY n;
SELECT dolt_checkout('mine');
SELECT try($$SELECT dolt_commit('--allow-empty', '-m', 'mine')$$) AS commit_own,
	try($$SELECT dolt_reset('--soft', 'HEAD~1')$$) AS soft_reset_own,
	try($$SELECT dolt_reset('--hard')$$) AS hard_reset_own,
	try($$SELECT dolt_merge('feat')$$) AS merge_into_own,
	try($$SELECT dolt_cherry_pick('feat')$$) AS pick_onto_own,
	try($$SELECT dolt_commit('--allow-empty', '-m', 'mine')$$,
		$$SELECT dolt_rebase('feat')$$) AS rebase_own,
	try($$SELECT dolt_integrate('-m', 'in')$$) AS integrate_into_main;
SELECT dolt_checkout('main');
SELECT try($$SELECT dolt_branch('-m', 'mine2', 'mine3')$$) AS rename_own,
	try($$SELECT dolt_branch('-D', 'mine2')$$) AS delete_own,
	try($$SELECT dolt_branch('-f', 'mine2', 'feat')$$) AS force_own;
-- a branch's owner is kept by its new name
SELECT dolt_branch('-m', 'mine2', 'mine3');
SELECT try($$SELECT dolt_branch('-D', 'mine3')$$) AS delete_renamed;
RESET ROLE;

-- a branch deleted and made again by another isn't the old owner's
SELECT dolt_branch('-D', 'mine3');
SELECT dolt_branch('mine3');
SET ROLE regress_hm_owner;
SELECT try($$SELECT dolt_branch('-D', 'mine3')$$) AS delete_others;
RESET ROLE;

-- dolt.transaction_commit's commit is a head move too
SET ROLE regress_hm_inserter;
SET dolt.transaction_commit = on;
INSERT INTO hm.t VALUES (100);
RESET dolt.transaction_commit;
RESET ROLE;
SELECT count(*) AS not_inserted FROM hm.t WHERE a = 100;

\c contrib_regression
DROP DATABASE regress_head_moves;
DROP ROLE regress_hm_reader, regress_hm_inserter, regress_hm_writer,
	regress_hm_creator, regress_hm_owner;
