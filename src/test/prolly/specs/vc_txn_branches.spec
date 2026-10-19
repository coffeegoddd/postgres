# Dolt's branch isolation, branch and auto-increment multi-session tests
# (BranchIsolationTests 1, 2 and 4, DoltBranchMultiSessionScriptTests 5,
# AutoIncrementTransactionTests 0), rederived under Postgres semantics: in
# READ COMMITTED a transaction sees other branches' heads, and the
# branches there are, as each statement begins (Dolt's as the transaction
# began: Postgres's REPEATABLE READ, which isn't run here, since listing a
# branch first used by another session after the snapshot fails there); a
# branch made in a transaction is another session's once it commits; and
# a sequence gives two transactions different values, neither waiting.

setup		{ CREATE SCHEMA vtb VERSIONED; }
setup
{
	CREATE TABLE vtb.t (id serial PRIMARY KEY, v int);
	INSERT INTO vtb.t (v) VALUES (0);
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vtb base'); END $$; }
setup		{ DO $$ BEGIN PERFORM dolt_branch('vtb_other'); END $$; }

teardown
{
	DO $$
	BEGIN
		PERFORM dolt_checkout('main');
		DROP SCHEMA vtb CASCADE;
		IF EXISTS (SELECT FROM dolt_branches WHERE name = 'vtb_other') THEN
			PERFORM dolt_branch('-D', 'vtb_other');
		END IF;
		IF EXISTS (SELECT FROM dolt_branches WHERE name = 'vtb_new') THEN
			PERFORM dolt_branch('-D', 'vtb_new');
		END IF;
		IF EXISTS (SELECT FROM dolt_branches WHERE name = 'vtb_mine') THEN
			PERFORM dolt_branch('-D', 'vtb_mine');
		END IF;
		PERFORM dolt_commit('-Am', 'vtb gone');
	END $$;
}

session a
step a_begin	{ BEGIN; }
step a_other_head { SELECT hash = dolt_hashof('main') AS other_at_base FROM dolt_branches WHERE name = 'vtb_other'; }
step a_branches	{ SELECT count(*) AS vtb_branches FROM dolt_branches WHERE starts_with(name, 'vtb_'); }
step a_named	{ SELECT name FROM dolt_branches WHERE name IN ('vtb_other', 'vtb_new') ORDER BY name; }
step a_join		{ SELECT b.name FROM (VALUES ('vtb_other'), ('vtb_new'), ('vtb_none')) AS w (name) JOIN dolt_branches b USING (name) ORDER BY 1; }
step a_mine		{ SELECT (dolt_checkout('-b', 'vtb_mine'))[1] = '0' AS ok; }
step a_active	{ SELECT active_branch(); }
step a_main		{ SELECT (dolt_checkout('main'))[1] = '0' AS ok; }
step a_insert	{ INSERT INTO vtb.t (v) VALUES (1) RETURNING id; }
step a_commit	{ COMMIT; }
# back on main, whatever branch a permutation left it on: a branch it is
# on can't be deleted, and the teardown deletes them all
teardown		{ DO $$ BEGIN PERFORM dolt_checkout('main'); END $$; }

session b
step b_begin	{ BEGIN; }
step b_on_other	{ SELECT (dolt_checkout('vtb_other'))[1] = '0' AS ok; }
step b_change	{ INSERT INTO vtb.t (v) VALUES (2); }
step b_dcommit	{ SELECT length((dolt_commit('-am', 'other moves'))[1]) = 40 AS committed; }
step b_main		{ SELECT (dolt_checkout('main'))[1] = '0' AS ok; }
step b_branch	{ SELECT dolt_branch('vtb_new'); }
step b_swap		{ SELECT dolt_branch('-d', 'vtb_other') AS deleted, dolt_branch('vtb_new') AS made; }
step b_branches	{ SELECT name FROM dolt_branches WHERE starts_with(name, 'vtb_') ORDER BY name; }
step b_delete_mine { SELECT dolt_branch('-D', 'vtb_mine'); }
step b_insert	{ INSERT INTO vtb.t (v) VALUES (2) RETURNING id; }
step b_commit	{ COMMIT; }
# back on main, as a is
teardown		{ DO $$ BEGIN PERFORM dolt_checkout('main'); END $$; }

# another branch's head moved since the transaction began (BranchIsolation-
# Tests 1): seen moved
permutation a_begin a_other_head b_on_other b_change b_dcommit b_main a_other_head a_commit
# a session left on a branch, one it made or another: back on main before
# the teardown deletes it, and nothing left for the next permutation
permutation a_mine a_active
permutation b_on_other
# a branch made since (2): seen made
permutation a_begin a_branches b_branch a_branches a_commit
# a branch deleted and another made since (4): seen by name, in a join,
# deleted and made
permutation a_begin a_named a_join b_swap a_named a_join a_commit
# a branch made, and checked out, in a transaction (DoltBranchMulti-
# SessionScriptTests 5): another session doesn't see it until it commits,
# and its forced delete waits for that commit, then is refused, the
# branch being the first session's (Dolt's sessions see it at once, and
# its forced delete takes it from under the session: D5, D8)
permutation a_begin a_mine b_branches b_delete_mine a_commit b_branches a_active a_main
# two transactions' sequence values (AutoIncrementTransactionTests 0):
# different, neither waiting
permutation a_begin b_begin a_insert b_insert a_commit b_commit
