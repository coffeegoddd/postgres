# Version control from many backends: a change to a branch locks its row
# (pg_history.pointers) before it reads it, so two on one branch take
# turns and neither is lost; changes to different branches never wait on
# each other.  A reset or a deletion racing a commit, and two making one
# branch, each end one way or the other, never half.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	SELECT test_vc_open();
	SELECT test_vc_commit('main', 'base');
	SELECT test_vc_branch('a', 'main');
	SELECT test_vc_branch('b', 'main');
}

# (the store as the test found it, for the tests after it)
teardown
{
	SELECT test_vc_clear() > 0 AS cleared;
}

session s1
step s1_begin	{ BEGIN; }
step s1_main	{ SELECT test_vc_commit('main', 's1'); }
step s1_a		{ SELECT test_vc_commit('a', 's1a'); }
step s1_hard	{ SELECT test_vc_reset('main', 'base', true); }
step s1_soft	{ SELECT test_vc_reset('main', 'base', false); }
step s1_drop_b	{ SELECT test_vc_branch_delete('b'); }
step s1_drop_a	{ SELECT test_vc_branch_delete('a'); }
step s1_make_x	{ SELECT test_vc_branch('x', 'main'); }
step s1_commit	{ COMMIT; }
step s1_rollback	{ ROLLBACK; }

session s2
step s2_main	{ SELECT test_vc_commit('main', 's2'); }
step s2_b		{ SELECT test_vc_commit('b', 's2b'); }
step s2_hard	{ SELECT test_vc_reset('main', 'base', true); }
step s2_soft	{ SELECT test_vc_reset('main', 'base', false); }
step s2_make_x	{ SELECT test_vc_branch('x', 'main'); }
step s2_rr		{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT test_vc_log('main'); }
step s2_rr_main	{ SELECT test_vc_commit('main', 's2'); }
step s2_end		{ ROLLBACK; }

session c
step c_log
{
	SELECT test_vc_log('main') AS main, test_vc_log('a') AS a,
		test_vc_log('b') AS b, test_vc_log('x') AS x,
		test_vc_head_in_window('main') AS kept;
}
step c_s1_working	{ SELECT test_vc_working_has('main', 's1') AS has_s1; }

# two commits on one branch take turns; both land, the second on the first,
# its tree with the first's change in it
permutation s1_begin s1_main s2_main s1_commit c_log c_s1_working
# the first rolled back, the second lands on what was there
permutation s1_begin s1_main s2_main s1_rollback c_log c_s1_working
# changes to different branches do not wait on each other: a commit, a
# deletion, a reset, a branch made, beside a commit on another branch
permutation s1_begin s1_a s2_b s1_commit c_log
permutation s1_begin s1_drop_a s2_b s1_commit c_log
permutation s1_begin s1_hard s2_b s1_commit c_log
permutation s1_begin s1_make_x s2_b s1_commit c_log
# in repeatable read, the second fails cleanly, and nothing of it lands
permutation s1_begin s2_rr s1_main s2_rr_main s1_commit s2_end c_log

# a hard reset racing a commit: whichever locks first goes first
permutation s1_begin s1_main s2_hard s1_commit c_log c_s1_working
permutation s1_begin s1_hard s2_main s1_commit c_log
# a soft reset after a commit keeps the commit's change, uncommitted
permutation s1_begin s1_main s2_soft s1_commit c_log c_s1_working
permutation s1_begin s1_soft s2_main s1_commit c_log

# a commit racing its branch's deletion fails once the deletion commits
permutation s1_begin s1_drop_b s2_b s1_commit c_log
permutation s1_begin s1_drop_b s2_b s1_rollback c_log

# two making one branch: one makes it, the other finds it
permutation s1_begin s1_make_x s2_make_x s1_commit c_log
permutation s1_begin s1_make_x s2_make_x s1_rollback c_log
