# REPEATABLE READ: a snapshot doesn't see later commits, and updating a row
# another transaction changed after the snapshot fails with a serialization
# error.

setup
{
	CREATE TABLE rr (id int PRIMARY KEY, v int);
	INSERT INTO rr VALUES (1, 10), (2, 20);
}

teardown
{
	DROP TABLE rr;
}

session s1
step s1_begin		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_read		{ SELECT * FROM rr ORDER BY id; }
step s1_update		{ UPDATE rr SET v = v + 1 WHERE id = 1; }
step s1_update2		{ UPDATE rr SET v = v + 1 WHERE id = 2; }
step s1_commit		{ COMMIT; }

session s2
step s2_update		{ UPDATE rr SET v = v * 100 WHERE id = 1; }
step s2_insert		{ INSERT INTO rr VALUES (3, 30); }

permutation s1_begin s1_read s2_update s2_insert s1_read s1_update s1_commit
permutation s1_begin s1_read s2_update s1_update2 s1_read s1_commit
