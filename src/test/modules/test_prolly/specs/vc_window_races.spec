# A sever racing a call that points a branch at an old commit: the call
# locks the repository's row for share, the sever for update, so they take
# turns, and whichever goes second sees what the first did.  A reset, or a
# branch made, at a commit the sever cut is refused; a sever after one
# keeps the commit, a head.  A fast-forward merge too.  The window's calls
# refuse a transaction's snapshot.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	SELECT test_vc_open();
	SELECT test_vc_commit('main', 'base');
	SELECT test_vc_commit('main', 'c1');
	SELECT test_vc_commit('main', 'c2');
	SELECT test_vc_commit('main', 'c3');
	SELECT test_vc_branch('ahead', 'main');
	SELECT test_vc_commit('ahead', 'a1');
}

# (the store as the test found it, for the tests after it)
teardown
{
	SELECT test_vc_clear() > 0 AS cleared;
}

session s1
step s1_begin	{ BEGIN; }
step s1_reset	{ SELECT test_vc_reset('main', 'base', true); }
step s1_sever	{ SELECT test_vc_sever(1) AS cut; }
step s1_old		{ SELECT test_vc_branch_at('old', 'main', 'base'); }
step s1_ff		{ SELECT test_vc_merge('main', 'ahead'); }
step s1_rr		{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1; }
step s1_rollback	{ ROLLBACK; }
step s1_commit	{ COMMIT; }

session s2
step s2_sever	{ SELECT test_vc_sever(1) AS cut; }
step s2_reset	{ SELECT test_vc_reset('main', 'base', true); }
step s2_old		{ SELECT test_vc_branch_at('old', 'main', 'base'); }
step s2_ff		{ SELECT test_vc_merge('main', 'ahead'); }

session c
step c_repo
{
	SELECT test_vc_head_in_window('main') AS kept,
		test_vc_head_in_window('old') AS old_kept,
		(SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'vc/commit/%') AS commits;
}

# the reset first: the sever waits, then keeps base, the head now
permutation s1_begin s1_reset s2_sever s1_commit c_repo
# the sever first: base is cut, and the reset to it, waiting, finds it gone
permutation s1_begin s1_sever s2_reset s1_commit c_repo
# a branch made at an old commit, either way
permutation s1_begin s1_old s2_sever s1_commit c_repo
permutation s1_begin s1_sever s2_old s1_commit c_repo
# a fast-forward merge to ahead's head, either way: the head moved to is kept
permutation s1_begin s1_ff s2_sever s1_commit c_repo
permutation s1_begin s1_sever s2_ff s1_commit c_repo
# under a transaction's snapshot, a sever refuses to run
permutation s1_rr s1_sever s1_rollback c_repo
