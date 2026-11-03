# Two backends opening an empty repository at once: one makes "main" and
# its init commit; the other finds them, and keeps nothing of its own.
# When both write one init commit (one microsecond: one hash), its row is
# the first's, and stays: the head keeps its row.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
}

# (the store as the test found it, for the tests after it)
teardown
{
	SELECT test_vc_clear() > 0 AS cleared;
}

# (each permutation its own times, later than the last's: a backend's init
# commits are never earlier than its last, whatever the clock says)
session s1
step s1_begin	{ BEGIN; }
step s1_open	{ SELECT test_vc_open(1000); }
step s1_open2	{ SELECT test_vc_open(5000); }
step s1_open3	{ SELECT test_vc_open(9000); }
step s1_commit	{ COMMIT; }
step s1_rollback	{ ROLLBACK; }

session s2
step s2_same	{ SELECT test_vc_open(1000); }
step s2_later	{ SELECT test_vc_open(6000); }
step s2_later3	{ SELECT test_vc_open(10000); }

session c
step c_repo
{
	SELECT test_vc_log('main') AS main, test_vc_head_in_window('main') AS kept,
		(SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'vc/commit/%') AS commits;
}

# one microsecond, one init commit: its row stays, the head keeps it
permutation s1_begin s1_open s2_same s1_commit c_repo
# two init commits: the second's is let go of, with its row
permutation s1_begin s1_open2 s2_later s1_commit c_repo
# the first rolled back, the second makes the repository
permutation s1_begin s1_open3 s2_later3 s1_rollback c_repo
