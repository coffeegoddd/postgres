# Dolt's multi-session commit tests (DoltStoredProcedureTransactionTests
# 1-2 and 7-8, DoltConflictHandlingTests 3-6, DoltConstraintViolation-
# TransactionTests 1), rederived under Postgres semantics, with savepoints
# around version-control calls.  A prolly commit is of the branch's
# working set as the committing transaction sees it: another session's
# uncommitted writes aren't in it, and stay uncommitted changes once that
# session commits.  Two transactions committing to one branch are
# serialized on the branch: the second waits for the first, then, in READ
# COMMITTED, commits on top of it, its fold reading the new head's rows;
# in REPEATABLE READ its snapshot can't, and it fails with a serialization
# failure.  Neither loses the other's changes (Dolt's later commit merges
# its working set into the moved head).
# Conflicts and violations a merge leaves can't be committed unless
# dolt.allow_commit_conflicts is on.

setup		{ CREATE SCHEMA vtc VERSIONED; }
setup
{
	CREATE TABLE vtc.users (id int PRIMARY KEY, name text);
	INSERT INTO vtc.users VALUES (1, 'tim'), (2, 'jim');
	CREATE TABLE vtc.parent (pk int PRIMARY KEY);
	CREATE TABLE vtc.child (pk int PRIMARY KEY, p int REFERENCES vtc.parent);
	INSERT INTO vtc.parent VALUES (1), (2);
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vtc base'); END $$; }
setup		{ DO $$ BEGIN PERFORM dolt_branch('vtc_side'); END $$; }
setup
{
	DO $$
	BEGIN
		PERFORM dolt_checkout('vtc_side');
		UPDATE vtc.users SET name = 'side' WHERE id = 1;
		INSERT INTO vtc.child VALUES (2, 2);
		PERFORM dolt_commit('-am', 'vtc side');
		PERFORM dolt_checkout('main');
	END $$;
}

teardown
{
	DO $$
	BEGIN
		PERFORM dolt_checkout('main');
		PERFORM dolt_reset('--hard', 'HEAD');
		DROP SCHEMA vtc CASCADE;
		PERFORM dolt_branch('-D', 'vtc_side');
		PERFORM dolt_commit('-Am', 'vtc gone');
	END $$;
}

session a
step a_begin	{ BEGIN; }
step a_tim		{ UPDATE vtc.users SET name = 'tim2' WHERE id = 1; }
step a_dcommit	{ SELECT length((dolt_commit('-Am', 'a commits'))[1]) = 64 AS committed; }
step a_commit	{ COMMIT; }
step a_merge	{ SELECT (dolt_merge('vtc_side'))[2:4]; }
step a_pick		{ SELECT (dolt_cherry_pick('vtc_side'))[2:4]; }
step a_conflicts { SELECT table_name, num_conflicts FROM dolt_conflicts WHERE table_name LIKE 'vtc.%'; }
step a_allow	{ SET LOCAL dolt.allow_commit_conflicts = on; }
step a_savepoint { SAVEPOINT before_merge; }
step a_to_savepoint { ROLLBACK TO SAVEPOINT before_merge; }
step a_del_parent2 { DELETE FROM vtc.parent WHERE pk = 2; }
step a_violations { SELECT table_name, num_violations FROM dolt_constraint_violations() WHERE table_name LIKE 'vtc.%'; }
step a_status	{ SELECT table_name, status FROM dolt_status WHERE table_name LIKE 'vtc.%'; }
step a_log		{ SELECT message FROM dolt_log ORDER BY commit_order DESC LIMIT 1; }
step a_read		{ SELECT string_agg(id || ':' || name, ',' ORDER BY id) AS users FROM vtc.users; }

session b
step b_begin	{ BEGIN; }
step b_rr		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step b_jim		{ UPDATE vtc.users SET name = 'jim2' WHERE id = 2; }
step b_dcommit	{ SELECT length((dolt_commit('-Am', 'b commits'))[1]) = 64 AS committed; }
step b_commit	{ COMMIT; }
step b_status	{ SELECT table_name, status FROM dolt_status WHERE table_name LIKE 'vtc.%'; }
step b_head_users { SELECT string_agg((to_row).id || ':' || (to_row).name, ',' ORDER BY (to_row).id) AS committed_change FROM dolt_diff('HEAD~1', 'HEAD', NULL::vtc.users); }

# a commit doesn't take another session's uncommitted writes
# (DoltStoredProcedureTransactionTests 2): b commits jim's change alone;
# tim's, once a commits it, is an uncommitted change
permutation a_begin a_tim b_jim b_dcommit b_head_users a_commit b_status a_log
# both commit in transactions (1): the second waits for the first, then
# commits on top of it; in REPEATABLE READ, fails
permutation a_begin b_begin a_tim b_jim a_dcommit b_dcommit a_commit b_commit a_log a_read b_head_users
permutation a_begin b_rr a_tim b_jim a_dcommit b_dcommit a_commit b_commit a_log a_read b_head_users
# the head moved by another session while a transaction holds changes
# (7, 8): its later commit waits for nothing and goes on from the new head
permutation a_begin a_tim b_jim b_dcommit a_dcommit a_commit a_log b_status
# a merge's conflicts in a transaction block (DoltConflictHandlingTests
# 3-5): listed; not committed unless dolt.allow_commit_conflicts is on
permutation a_tim a_dcommit a_begin a_merge a_conflicts a_commit a_conflicts
permutation a_tim a_dcommit a_begin a_allow a_merge a_commit a_conflicts a_status
# a savepoint around the merge: rolled back to it, the merge and its
# conflicts go, and the transaction commits
permutation a_tim a_dcommit a_begin a_savepoint a_merge a_conflicts a_to_savepoint a_conflicts a_commit a_read
# violations a merge leaves (DoltConstraintViolationTransactionTests 1):
# not committed either
permutation a_del_parent2 a_dcommit a_begin a_merge a_violations a_commit a_violations
# the head moved by another session around a cherry-pick in a
# transaction: the other's commit waits for the pick's transaction, then
# goes on from the pick's commit
permutation a_begin a_pick b_jim b_dcommit a_commit a_log a_read
