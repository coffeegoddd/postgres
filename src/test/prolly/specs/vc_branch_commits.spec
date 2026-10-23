# Version control: a commit holds its own branch's refs, not every ref.
# Commits on different branches don't wait for each other's open
# transactions, and each keeps the other's commit; a second committer on
# one branch waits for the first and builds on it; a branch made by a
# session on a branch with an open commit (a change of other refs, holding
# the session's branch too) waits for it, and is made at its commit.

setup		{ CREATE SCHEMA vbc VERSIONED; }
setup
{
	CREATE TABLE vbc.t (id int PRIMARY KEY, v text);
	INSERT INTO vbc.t VALUES (1, 'base');
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vbc base'); END $$; }
setup		{ DO $$ BEGIN PERFORM dolt_branch('vbc_b'); END $$; }

teardown
{
	DO $$
	BEGIN
		PERFORM dolt_checkout('main');
		DROP SCHEMA vbc CASCADE;
		PERFORM dolt_branch('-D', 'vbc_b');
		PERFORM dolt_branch('-D', 'vbc_c');
		PERFORM dolt_commit('-Am', 'vbc gone');
	END $$;
}

session s1
step s1_main	{ SELECT (dolt_checkout('main'))[1] = '0' AS ok; }
step s1_begin	{ BEGIN; }
step s1_commit	{ INSERT INTO vbc.t VALUES (2, 's1'); SELECT dolt_commit('-am', 'vbc s1') IS NOT NULL AS ok; }
step s1_end		{ COMMIT; }

session s2
step s2_b		{ SELECT (dolt_checkout('vbc_b'))[1] = '0' AS ok; }
step s2_main	{ SELECT (dolt_checkout('main'))[1] = '0' AS ok; }
step s2_commit	{ INSERT INTO vbc.t VALUES (3, 's2'); SELECT dolt_commit('-am', 'vbc s2') IS NOT NULL AS ok; }
step s2_back	{ SELECT (dolt_checkout('main'))[1] = '0' AS ok; }

session s3
step s3_branch	{ SELECT (dolt_branch('vbc_c'))[1] = '0' AS ok; }
step s3_logs
{
	SELECT (SELECT string_agg(message, ',') FROM
			(SELECT message FROM dolt_log('main') WHERE message LIKE 'vbc %' LIMIT 3) l) AS main_log,
		(SELECT string_agg(message, ',') FROM
			(SELECT message FROM dolt_log('vbc_b') WHERE message LIKE 'vbc %' LIMIT 2) l) AS b_log,
		dolt_hashof('vbc_c') = dolt_hashof('main') AS c_at_main;
}

# different branches: s2's commit doesn't wait for s1's open one
permutation s1_main s2_b s1_begin s1_commit s2_commit s1_end s2_back s3_branch s3_logs

# one branch: s2 waits for s1, then commits on top of it
permutation s1_main s2_main s1_begin s1_commit s2_commit s1_end s3_branch s3_logs

# a branch creation on main waits for main's open commit, made at it
permutation s1_main s1_begin s1_commit s3_branch s1_end s3_logs
