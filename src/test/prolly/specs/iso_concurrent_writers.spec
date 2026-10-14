# Transactions that have each started writing a table (an UPDATE that
# matched no rows still holds the table's write-mode lock) both go on to
# insert into it: neither fails, and both rows are there once they commit.

setup
{
	CREATE TABLE cw (id int PRIMARY KEY, v int);
	INSERT INTO cw VALUES (1, 0);
}

teardown
{
	DROP TABLE cw;
}

session s1
step s1_begin		{ BEGIN; }
step s1_noop		{ UPDATE cw SET v = v WHERE id = -1; }
step s1_insert		{ INSERT INTO cw VALUES (2, 1); }
step s1_commit		{ COMMIT; }

session s2
step s2_begin		{ BEGIN; }
step s2_noop		{ UPDATE cw SET v = v WHERE id = -1; }
step s2_insert		{ INSERT INTO cw VALUES (3, 2); }
step s2_commit		{ COMMIT; }
step s2_select		{ SELECT * FROM cw ORDER BY id; }

permutation s1_begin s2_begin s1_noop s2_noop s1_insert s2_insert s1_commit s2_commit s2_select
permutation s1_begin s2_begin s1_noop s2_noop s2_insert s1_insert s2_commit s1_commit s2_select
