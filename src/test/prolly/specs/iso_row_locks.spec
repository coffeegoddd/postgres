# Row locks on engine tables: FOR UPDATE blocks a second locker until
# commit; NOWAIT errors; SKIP LOCKED skips; FOR KEY SHARE doesn't block
# FOR NO KEY UPDATE.

setup
{
	CREATE TABLE locks (id int PRIMARY KEY, v int);
	INSERT INTO locks SELECT g, g FROM generate_series(1, 5) g;
}

teardown
{
	DROP TABLE locks;
}

session s1
step s1_begin		{ BEGIN; }
step s1_lock		{ SELECT id FROM locks WHERE id = 1 FOR UPDATE; }
step s1_keyshare	{ SELECT id FROM locks WHERE id = 2 FOR KEY SHARE; }
step s1_commit		{ COMMIT; }

session s2
step s2_begin		{ BEGIN; }
step s2_lock		{ SELECT id FROM locks WHERE id = 1 FOR UPDATE; }
step s2_nowait		{ SELECT id FROM locks WHERE id = 1 FOR UPDATE NOWAIT; }
step s2_skip		{ SELECT id FROM locks WHERE id <= 3 ORDER BY id FOR UPDATE SKIP LOCKED; }
step s2_nokeyupd	{ UPDATE locks SET v = v + 100 WHERE id = 2 RETURNING v; }
step s2_commit		{ COMMIT; }

permutation s1_begin s1_lock s2_begin s2_lock s1_commit s2_commit
permutation s1_begin s1_lock s2_begin s2_nowait s1_commit s2_commit
permutation s1_begin s1_lock s2_begin s2_skip s1_commit s2_commit
permutation s1_begin s1_keyshare s2_begin s2_nokeyupd s2_commit s1_commit
