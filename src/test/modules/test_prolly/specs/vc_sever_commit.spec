# A commit landing while a sever runs, between its listing of the commits'
# references and its reading of the heads: the sever lists before it reads,
# and a commit's reference and its branch's move become visible together
# as its transaction commits, so the new head is in neither the listing
# nor the cut, and keeps its reference.  Two commits landing so: the one
# below the new head, in the window but not the listing, is walked through
# (it is no commit cut), and the commits below it the window keeps stay.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	SELECT test_vc_open();
	SELECT test_vc_commit('main', 'base');
	SELECT test_vc_commit('main', 'c1');
}

# (the store as the test found it, for the tests after it)
teardown
{
	SELECT test_vc_clear() > 0 AS cleared;
}

session s1
setup			{ SELECT injection_points_set_local(); }
step s1_attach	{ SELECT injection_points_attach('vc-sever-listed', 'wait'); }
step s1_sever	{ SELECT test_vc_sever(1) AS cut; }
step s1_sever3	{ SELECT test_vc_sever(3) AS cut; }
step s1_detach	{ SELECT injection_points_detach('vc-sever-listed'); }

session s2
step s2_commit	{ SELECT test_vc_commit('main', 'c2'); }
step s2_commit3	{ SELECT test_vc_commit('main', 'c3'); }
step s2_wake	{ SELECT injection_points_wakeup('vc-sever-listed'); }

session c
step c_repo
{
	SELECT test_vc_head_in_window('main') AS kept,
		(SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'vc/commit/%') AS commits;
}

permutation s1_attach s1_sever s2_commit s2_wake s1_detach c_repo
permutation s1_attach s1_sever3 s2_commit s2_commit3 s2_wake s1_detach c_repo
