# Version control: a branch's first use, when its head's pages are kept
# (by the branch it was made from, as of its commit), makes its tables of
# those pages: it waits for no writer of the source, and no writer of the
# source waits for it.  A transaction writing the source while the branch
# is first used holds it up not at all, and its rows (committed or rolled
# back after) are never the branch's.

setup		{ CREATE SCHEMA vfk VERSIONED; }
setup
{
	CREATE TABLE vfk.t (id int PRIMARY KEY, v text);
	INSERT INTO vfk.t SELECT g, 'base' FROM generate_series(1, 20) g;
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vfk base'); END $$; }
setup		{ DO $$ BEGIN PERFORM dolt_branch('vfk_b'); END $$; }

teardown
{
	DO $$
	BEGIN
		PERFORM dolt_checkout('main');
		PERFORM dolt_branch('-D', 'vfk_b');
		PERFORM dolt_reset('--hard');
		DROP SCHEMA vfk CASCADE;
		PERFORM dolt_commit('-Am', 'vfk gone');
	END $$;
}

session s1
step s1_begin	{ BEGIN; }
step s1_write	{ UPDATE vfk.t SET v = 's1' WHERE id = 1; }
step s1_commit	{ COMMIT; }
step s1_abort	{ ROLLBACK; }

session s2
setup			{ SET lock_timeout = '2s'; }
step s2_first	{ SELECT (dolt_checkout('vfk_b'))[1] = '0' AS ok; }
step s2_read	{ SELECT string_agg(DISTINCT v, ',') AS v FROM vfk.t; }
step s2_main	{ SELECT (dolt_checkout('main'))[1] = '0' AS ok; }

session s3
setup			{ SET lock_timeout = '50ms'; }
step s3_write	{ UPDATE vfk.t SET v = 's3' WHERE id = 11; }
step s3_read	{ SELECT string_agg(v, ',' ORDER BY id) FILTER (WHERE id IN (1, 11)) AS v FROM vfk.t; }

# a writer in flight on main: the first use doesn't wait, nor does a
# writer after it; neither's row is on the branch
permutation s1_begin s1_write s2_first s3_write s2_read s1_commit s3_read s2_main
permutation s1_begin s1_write s2_first s1_abort s2_read s3_read s2_main
