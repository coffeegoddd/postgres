# A rewriting sever moves heads, so it locks every branch first: it waits
# for a commit in flight on any, and then keeps that commit (its row and
# its place on its branch's line, written again); a commit begun while
# the sever runs waits for it, and lands on the head it wrote.  The mode
# set, or a branch made, while the sever waits for the repository's state
# would have it lock a branch after the state: it is refused instead.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	SELECT test_vc_open();
	SELECT test_vc_commit('main', 'base');
	SELECT test_vc_commit('main', 'c1');
	SELECT test_vc_branch('b', 'main');
	SELECT test_vc_commit('b', 'b1');
	SELECT test_vc_rewrite(true);
}

# (the store as the test found it, for the tests after it)
teardown
{
	SELECT test_vc_clear() > 0 AS cleared;
}

session s1
step s1_begin	{ BEGIN; }
step s1_sever	{ SELECT test_vc_sever(1) AS cut; }
step s1_commit	{ COMMIT; }

session s2
step s2_begin	{ BEGIN; }
step s2_main	{ SELECT test_vc_commit('main', 'c2'); }
step s2_b		{ SELECT test_vc_commit('b', 'b2'); }
step s2_commit	{ COMMIT; }

session s3
step s3_keep	{ SELECT test_vc_rewrite(false); }
step s3_begin	{ BEGIN; }
step s3_rewrite	{ SELECT test_vc_rewrite(true); }
step s3_branch	{ SELECT test_vc_branch('c', 'main'); }
step s3_commit	{ COMMIT; }

session c
step c_repo
{
	SELECT test_vc_log('main') AS main, test_vc_log('b') AS b,
		test_vc_working_has('main', 'c2') AS has_c2,
		(SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'vc/commit/%') AS commits;
}

# a commit in flight on main: the sever waits for it, and keeps it as the root
permutation s2_begin s2_main s1_sever s2_commit c_repo
# a commit on b while the sever runs: it waits, and lands on b's new root
permutation s1_begin s1_sever s2_b s1_commit c_repo
# the mode set to REWRITE while a sever, which read KEEP, waits: refused
permutation s3_keep s3_begin s3_rewrite s1_sever s3_commit c_repo
# a branch made while a rewriting sever waits: refused
permutation s3_begin s3_branch s1_sever s3_commit c_repo
