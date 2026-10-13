# SERIALIZABLE: classic write skew (each transaction reads what the other
# writes) makes one of them fail.

setup
{
	CREATE TABLE oncall (doctor text PRIMARY KEY, on_call bool);
	INSERT INTO oncall VALUES ('alice', true), ('bob', true);
}

teardown
{
	DROP TABLE oncall;
}

session s1
step s1_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_check	{ SELECT count(*) FROM oncall WHERE on_call; }
step s1_leave	{ UPDATE oncall SET on_call = false WHERE doctor = 'alice'; }
step s1_commit	{ COMMIT; }

session s2
step s2_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_check	{ SELECT count(*) FROM oncall WHERE on_call; }
step s2_leave	{ UPDATE oncall SET on_call = false WHERE doctor = 'bob'; }
step s2_commit	{ COMMIT; }

session s3
step s3_show	{ SELECT * FROM oncall ORDER BY doctor; }

permutation s1_begin s2_begin s1_check s2_check s1_leave s2_leave s1_commit s2_commit s3_show
