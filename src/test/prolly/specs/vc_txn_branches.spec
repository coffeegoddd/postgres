# Dolt's branch isolation and auto-increment multi-session tests
# (BranchIsolationTests 1-2, AutoIncrementTransactionTests 0), rederived
# under Postgres semantics: in READ COMMITTED a transaction sees other
# branches' heads, and the branches there are, as each statement begins
# (Dolt's as the transaction began: Postgres's REPEATABLE READ, which
# isn't run here, since listing a branch first used by another session
# after the snapshot fails there); and a sequence gives two transactions
# different values, neither waiting.

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
		PERFORM dolt_branch('-D', 'vtb_other');
		IF EXISTS (SELECT FROM dolt_branches WHERE name = 'vtb_new') THEN
			PERFORM dolt_branch('-D', 'vtb_new');
		END IF;
		PERFORM dolt_commit('-Am', 'vtb gone');
	END $$;
}

session a
step a_begin	{ BEGIN; }
step a_other_head { SELECT hash = dolt_hashof('main') AS other_at_base FROM dolt_branches WHERE name = 'vtb_other'; }
step a_branches	{ SELECT count(*) AS vtb_branches FROM dolt_branches WHERE starts_with(name, 'vtb_'); }
step a_insert	{ INSERT INTO vtb.t (v) VALUES (1) RETURNING id; }
step a_commit	{ COMMIT; }

session b
step b_begin	{ BEGIN; }
step b_on_other	{ SELECT (dolt_checkout('vtb_other'))[1] = '0' AS ok; }
step b_change	{ INSERT INTO vtb.t (v) VALUES (2); }
step b_dcommit	{ SELECT length((dolt_commit('-am', 'other moves'))[1]) = 64 AS committed; }
step b_main		{ SELECT (dolt_checkout('main'))[1] = '0' AS ok; }
step b_branch	{ SELECT dolt_branch('vtb_new'); }
step b_insert	{ INSERT INTO vtb.t (v) VALUES (2) RETURNING id; }
step b_commit	{ COMMIT; }

# another branch's head moved since the transaction began (BranchIsolation-
# Tests 1): seen moved
permutation a_begin a_other_head b_on_other b_change b_dcommit b_main a_other_head a_commit
# a branch made since (2): seen made
permutation a_begin a_branches b_branch a_branches a_commit
# two transactions' sequence values (AutoIncrementTransactionTests 0):
# different, neither waiting
permutation a_begin b_begin a_insert b_insert a_commit b_commit
