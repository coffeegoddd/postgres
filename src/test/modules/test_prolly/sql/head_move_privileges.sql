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
CREATE ROLE regress_hm_dml;
GRANT USAGE ON SCHEMA hm TO PUBLIC;
GRANT SELECT ON hm.t TO regress_hm_reader;
GRANT SELECT, INSERT ON hm.t TO regress_hm_inserter;
GRANT ALL ON hm.t TO regress_hm_writer, regress_hm_creator, regress_hm_owner,
	regress_hm_dml;
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
-- (a rebase's own branch, made on the role's behalf, isn't renamed into a
-- branch of its choosing)
SELECT dolt_commit('--allow-empty', '-m', 'mine, to rebase') IS NOT NULL AS committed;
SELECT dolt_rebase('-i', 'feat') IS NOT NULL AS began;
SELECT active_branch();
SELECT dolt_branch('-m', 'dolt_rebase_mine', 'free_branch');
SELECT dolt_rebase('--abort') IS NOT NULL AS aborted;
SELECT dolt_reset('--hard', 'HEAD~1') IS NOT NULL AS reset;
SELECT dolt_checkout('main');
-- a rename takes a new name, as making a branch does: not the owner's
SELECT try($$SELECT dolt_branch('-m', 'mine2', 'mine3')$$) AS rename_own,
	try($$SELECT dolt_branch('-D', 'mine2')$$) AS delete_own,
	try($$SELECT dolt_branch('-f', 'mine2', 'feat')$$) AS force_own;
RESET ROLE;
-- a branch's owner is kept by its new name
SET ROLE regress_hm_writer;
SELECT dolt_branch('-m', 'mine2', 'mine3');
RESET ROLE;
SELECT name, CASE WHEN creator = current_user THEN '(database owner)' ELSE creator END AS creator
FROM dolt_branches WHERE name IN ('main', 'mine', 'mine3')
ORDER BY name;
SET ROLE regress_hm_owner;
SELECT try($$SELECT dolt_branch('-D', 'mine3')$$) AS delete_renamed;
RESET ROLE;

-- a branch deleted and made again by another isn't the old owner's
SELECT dolt_branch('-D', 'mine3');
SELECT dolt_branch('mine3');
SET ROLE regress_hm_owner;
SELECT try($$SELECT dolt_branch('-D', 'mine3')$$) AS delete_others;
RESET ROLE;

-- a branch whose maker's objects were dropped is the database owner's
CREATE ROLE regress_hm_gone;
GRANT CREATE ON DATABASE regress_head_moves TO regress_hm_gone;
SET ROLE regress_hm_gone;
SELECT dolt_branch('orphan');
RESET ROLE;
SELECT name, creator = 'regress_hm_gone' AS by_gone FROM dolt_branches
WHERE name = 'orphan';
REVOKE CREATE ON DATABASE regress_head_moves FROM regress_hm_gone;
DROP OWNED BY regress_hm_gone;
DROP ROLE regress_hm_gone;
SELECT name, creator = current_user AS database_owners FROM dolt_branches
WHERE name = 'orphan';

-- stashes: dropped (or popped) only by the role that pushed them, or one
-- that writes the refs; a clear clears only the role's own
INSERT INTO hm.t VALUES (50);
SET ROLE regress_hm_dml;
SELECT dolt_stash('push', 'shelf');
RESET ROLE;
INSERT INTO hm.t VALUES (51);
SELECT dolt_stash('push', 'shelf');
SELECT name, stash_id FROM dolt_stashes ORDER BY stash_id;
SET ROLE regress_hm_reader;
SELECT try($$SELECT dolt_stash('drop', 'shelf', 'stash@{1}')$$) AS drop_others,
	try($$SELECT dolt_stash('pop', 'shelf', 'stash@{1}')$$) AS pop_others,
	try($$SELECT dolt_stash('clear', 'shelf')$$) AS clear_none_own;
RESET ROLE;
SELECT count(*) AS kept FROM dolt_stashes;
SET ROLE regress_hm_dml;
SELECT try($$SELECT dolt_stash('drop', 'shelf', 'stash@{0}')$$) AS drop_others,
	try($$SELECT dolt_stash('drop', 'shelf', 'stash@{1}')$$) AS drop_own;
SELECT dolt_stash('clear', 'shelf');
RESET ROLE;
SELECT name, stash_id FROM dolt_stashes ORDER BY stash_id;
SET ROLE regress_hm_writer;
SELECT dolt_stash('clear', 'shelf');
RESET ROLE;
SELECT count(*) AS left FROM dolt_stashes;

-- tags: made by a role that writes the refs, deleted by their maker too
SET ROLE regress_hm_reader;
SELECT try($$SELECT dolt_tag('v_reader')$$) AS tag_reader;
RESET ROLE;
SET ROLE regress_hm_owner;
SELECT try($$SELECT dolt_tag('v_owner')$$) AS tag_branch_owner;
RESET ROLE;
SET ROLE regress_hm_creator;
SELECT dolt_tag('v_creator');
RESET ROLE;
SELECT dolt_tag('v_super');
SET ROLE regress_hm_inserter;
SELECT try($$SELECT dolt_tag('-d', 'v_creator')$$) AS delete_others;
RESET ROLE;
REVOKE CREATE ON DATABASE regress_head_moves FROM regress_hm_creator;
SET ROLE regress_hm_creator;
SELECT try($$SELECT dolt_tag('-d', 'v_super')$$) AS delete_others,
	try($$SELECT dolt_tag('-d', 'v_creator')$$) AS delete_own;
RESET ROLE;
GRANT CREATE ON DATABASE regress_head_moves TO regress_hm_creator;
SET ROLE regress_hm_writer;
SELECT try($$SELECT dolt_tag('-d', 'v_super')$$) AS delete_as_writer;
RESET ROLE;

-- remotes: added and removed by a role that writes the refs
SET ROLE regress_hm_reader;
SELECT try($$SELECT dolt_remote('add', 'other', 'file:///nowhere')$$) AS add_reader,
	try($$SELECT dolt_remote('remove', 'origin')$$) AS remove_reader;
RESET ROLE;
SET ROLE regress_hm_writer;
SELECT try($$SELECT dolt_remote('add', 'other', 'file:///nowhere')$$,
		   $$SELECT dolt_remote('remove', 'other')$$) AS add_remove_writer;
RESET ROLE;

-- a fetch sets the remote's tags here only for a role that writes the
-- refs: another's leaves them, and a fetched tag is no one's to delete
SELECT dolt_tag('v_remote') IS NOT NULL AS tagged;
SELECT dolt_push('origin', 'v_remote') IS NOT NULL AS pushed;
SELECT dolt_tag('-d', 'v_remote') IS NOT NULL AS deleted;
CREATE ROLE regress_hm_fetcher IN ROLE pg_read_server_files;
SET ROLE regress_hm_fetcher;
SELECT dolt_fetch('origin') IS NOT NULL AS fetched;
RESET ROLE;
SELECT count(*) AS v_remote_here FROM dolt_tags WHERE tag_name = 'v_remote';
SELECT dolt_fetch('origin') IS NOT NULL AS fetched;
SELECT count(*) AS v_remote_here FROM dolt_tags WHERE tag_name = 'v_remote';
SET ROLE regress_hm_fetcher;
SELECT try($$SELECT dolt_tag('-d', 'v_remote')$$) AS delete_fetched;
RESET ROLE;
DROP ROLE regress_hm_fetcher;

-- a forced move of a branch without commits keeps its owner, and is a
-- head move: its owner's, without CREATE
CREATE DATABASE regress_head_moves_empty;
\c regress_head_moves_empty
CREATE EXTENSION test_prolly;
CREATE SCHEMA hme VERSIONED;
GRANT CREATE ON DATABASE regress_head_moves_empty TO regress_hm_owner;
SET ROLE regress_hm_owner;
SELECT dolt_branch('empty1');
RESET ROLE;
REVOKE CREATE ON DATABASE regress_head_moves_empty FROM regress_hm_owner;
SET ROLE regress_hm_owner;
SELECT dolt_branch('-f', 'empty1', 'main');
RESET ROLE;
SET ROLE regress_hm_writer;
SELECT dolt_branch('-f', 'empty1', 'main');
RESET ROLE;
SELECT name, creator FROM dolt_branches WHERE name = 'empty1';
\c regress_head_moves

-- writer modes: a writer with its tables' grants alone integrates its own
-- writer branch where it may write the rows it changes as they changed; a
-- change to rows it may not write so takes a head move's privileges (and
-- the rows a move applies take all of a table's write privileges)
CREATE TABLE hm.w (id int PRIMARY KEY, v text);
INSERT INTO hm.w VALUES (1, 'one');
GRANT SELECT, INSERT, UPDATE, DELETE ON hm.w TO regress_hm_inserter;
CREATE TABLE hm.v (id int PRIMARY KEY, v text);
INSERT INTO hm.v VALUES (1, 'one');
GRANT SELECT, INSERT, DELETE ON hm.v TO regress_hm_inserter;
-- (keys from a serial and an identity column: their sequences' values move)
CREATE TABLE hm.ser (id serial PRIMARY KEY, v text);
CREATE TABLE hm.idt (id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY, v text);
GRANT SELECT, INSERT, UPDATE, DELETE ON hm.ser, hm.idt TO regress_hm_inserter;
GRANT USAGE ON SEQUENCE hm.ser_id_seq, hm.idt_id_seq TO regress_hm_inserter;
SELECT length(dolt_commit('-Am', 'w and v')::text) AS committed;
SELECT dolt_writer_mode('branch-auto');
SET ROLE regress_hm_inserter;
SET dolt.writer = 'plain';
INSERT INTO hm.w VALUES (2, 'plain');
SELECT active_branch();
INSERT INTO hm.ser (v) VALUES ('plain');
INSERT INTO hm.idt (v) VALUES ('plain');
SELECT length(dolt_commit('-am', 'plain row')::text) AS committed;
SELECT (dolt_integrate('-m', 'plain in'))[1] = dolt_hashof('main') AS integrated;
SELECT dolt_checkout('main');
SELECT id, v FROM hm.w ORDER BY id;
SELECT id, v FROM hm.ser;
SELECT id, v FROM hm.idt;
INSERT INTO hm.w VALUES (3, 'plain');
SELECT active_branch();
SELECT length(dolt_commit('-am', 'plain row 3')::text) AS committed;
RESET ROLE;
-- (a row of main's changed on the writer's branch, by the database's owner,
-- in a table the writer may not update)
SELECT dolt_checkout('writer/plain/main');
UPDATE hm.v SET v = 'changed' WHERE id = 1;
SELECT length(dolt_commit('-am', 'changed one')::text) AS committed;
SET ROLE regress_hm_inserter;
\set VERBOSITY terse
SELECT dolt_integrate('-m', 'in');
\set VERBOSITY default
RESET ROLE;
SELECT dolt_checkout('main');
SELECT (dolt_merge('-m', 'plain in', 'writer/plain/main'))[3] AS conflicts;
SELECT id, v FROM hm.w ORDER BY id;
SELECT id, v FROM hm.v ORDER BY id;
-- a writer branch's maker is a shared dependency: reassigned to another
-- role, the writer no longer integrates it alone, and reassigned back, it
-- does again
CREATE ROLE regress_hm_heir;
SET ROLE regress_hm_inserter;
INSERT INTO hm.w VALUES (4, 'reassigned');
SELECT active_branch();
SELECT length(dolt_commit('-am', 'plain row 4')::text) AS committed;
RESET ROLE;
REASSIGN OWNED BY regress_hm_inserter TO regress_hm_heir;
SET ROLE regress_hm_inserter;
\set VERBOSITY terse
SELECT dolt_integrate('-m', 'four in');
\set VERBOSITY default
RESET ROLE;
REASSIGN OWNED BY regress_hm_heir TO regress_hm_inserter;
SET ROLE regress_hm_inserter;
SELECT (dolt_integrate('-m', 'four in'))[1] = dolt_hashof('main') AS integrated;
RESET ROLE;
SELECT dolt_checkout('main');
RESET dolt.writer;
SELECT dolt_writer_mode('shared');

-- dolt.transaction_commit's commit is a head move too
SET ROLE regress_hm_inserter;
SET dolt.transaction_commit = on;
INSERT INTO hm.t VALUES (100);
RESET dolt.transaction_commit;
RESET ROLE;
SELECT count(*) AS not_inserted FROM hm.t WHERE a = 100;

\c contrib_regression
DROP DATABASE regress_head_moves;
DROP DATABASE regress_head_moves_empty;
DROP ROLE regress_hm_reader, regress_hm_inserter, regress_hm_writer,
	regress_hm_creator, regress_hm_owner, regress_hm_dml, regress_hm_heir;
