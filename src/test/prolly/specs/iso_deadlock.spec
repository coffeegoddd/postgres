# Two transactions that lock rows in opposite orders deadlock, and the
# deadlock detector breaks it.

setup
{
	CREATE TABLE dl (id int PRIMARY KEY, v int);
	INSERT INTO dl VALUES (1, 1), (2, 2);
}

teardown
{
	DROP TABLE dl;
}

session s1
setup				{ SET deadlock_timeout = '10ms'; }
step s1_begin		{ BEGIN; }
step s1_a			{ UPDATE dl SET v = v + 1 WHERE id = 1; }
step s1_b			{ UPDATE dl SET v = v + 1 WHERE id = 2; }
step s1_commit		{ COMMIT; }

session s2
setup				{ SET deadlock_timeout = '10s'; }
step s2_begin		{ BEGIN; }
step s2_b			{ UPDATE dl SET v = v + 10 WHERE id = 2; }
step s2_a			{ UPDATE dl SET v = v + 10 WHERE id = 1; }
step s2_commit		{ COMMIT; }

permutation s1_begin s2_begin s1_a s2_b s2_a(s1_b) s1_b(*) s1_commit s2_commit
