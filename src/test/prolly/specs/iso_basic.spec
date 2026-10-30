# A row inserted by one session is invisible to another until it commits,
# and a REPEATABLE READ snapshot taken before the commit never sees it.

setup
{
	CREATE TABLE iso_basic (id int PRIMARY KEY, v text);
}

teardown
{
	DROP TABLE iso_basic;
}

session writer
step w_begin	{ BEGIN; }
step w_insert	{ INSERT INTO iso_basic VALUES (1, 'one'); }
step w_commit	{ COMMIT; }

session reader
step r_count	{ SELECT count(*) FROM iso_basic; }

session snapshot
step s_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_basic; }
step s_count	{ SELECT count(*) FROM iso_basic; }
step s_commit	{ COMMIT; }

permutation w_begin s_begin w_insert r_count w_commit r_count s_count s_commit
