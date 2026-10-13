# READ COMMITTED: each statement sees what committed before it started; an
# update that waited for a concurrent update re-checks the new row version
# (EvalPlanQual) and applies to it.

setup
{
	CREATE TABLE rc (id int PRIMARY KEY, v int);
	INSERT INTO rc VALUES (1, 10), (2, 20);
}

teardown
{
	DROP TABLE rc;
}

session s1
step s1_begin		{ BEGIN; }
step s1_incr		{ UPDATE rc SET v = v + 1 WHERE id = 1; }
step s1_move		{ UPDATE rc SET v = 100 WHERE id = 2; }
step s1_commit		{ COMMIT; }

session s2
step s2_begin		{ BEGIN; }
step s2_read		{ SELECT * FROM rc ORDER BY id; }
step s2_incr		{ UPDATE rc SET v = v + 1 WHERE id = 1 RETURNING v; }
step s2_cond		{ UPDATE rc SET v = v + 1 WHERE v < 50 RETURNING id, v; }
step s2_commit		{ COMMIT; }

permutation s2_begin s2_read s1_begin s1_incr s2_read s1_commit s2_read s2_commit
permutation s1_begin s1_incr s2_begin s2_incr s1_commit s2_commit s2_read
permutation s1_begin s1_move s2_begin s2_cond s1_commit s2_commit s2_read
