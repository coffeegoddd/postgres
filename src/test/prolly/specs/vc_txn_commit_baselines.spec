# Under dolt.transaction_commit, each transaction's commit of its own rows
# advances the baseline where it held the old head's: two sessions
# committing to one branch, interleaved, leave a baseline that, with the
# log's remaining entries, folds to the working set (nothing to report in
# a status), reading only what's left, a keyless table's counts included.
# Read committed, the second commit's snapshot sees the first's fold, and
# advances from it; repeatable read, it doesn't, and keeps it (its own
# entries stay for the next fold).

setup		{ CREATE SCHEMA vtb VERSIONED; }
setup
{
	CREATE TABLE vtb.k (id int PRIMARY KEY, v text);
	CREATE TABLE vtb.n (v int);
	INSERT INTO vtb.k SELECT g, 'k' FROM generate_series(1, 100) g;
	INSERT INTO vtb.n SELECT g % 5 FROM generate_series(1, 100) g;
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vtb base'); END $$; }
setup		{ SELECT pg_stat_force_next_flush(); }

teardown
{
	DO $$
	BEGIN
		PERFORM dolt_reset('--hard', 'HEAD');
		DROP SCHEMA vtb CASCADE;
		PERFORM dolt_commit('-Am', 'vtb gone');
	END $$;
}

session a
setup		{ SET dolt.transaction_commit = on; }
step a_rc		{ BEGIN; }
step a_rr		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step a_write	{ UPDATE vtb.k SET v = 'a' WHERE id = 1; INSERT INTO vtb.n VALUES (1); }
step a_commit	{ COMMIT; }

session b
setup		{ SET dolt.transaction_commit = on; }
step b_rc		{ BEGIN; }
step b_rr		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step b_write	{ UPDATE vtb.k SET v = 'b' WHERE id = 2; INSERT INTO vtb.n VALUES (1); DELETE FROM vtb.n WHERE ctid = (SELECT min(ctid) FROM vtb.n WHERE v = 2); }
step b_commit	{ COMMIT; }

session c
step c_reset	{ SELECT pg_stat_reset_shared('prolly'); }
step c_status	{ SELECT count(*) AS changed FROM dolt_status; }
step c_flush	{ SELECT pg_stat_force_next_flush(); }
step c_full		{ SELECT value AS full_folds FROM pg_stat_prolly WHERE name = 'fold_full_tables'; }
step c_rows		{ SELECT (SELECT string_agg(v, ',' ORDER BY id) FROM vtb.k WHERE id <= 2) AS k, (SELECT count(*) FROM vtb.n) AS n, (SELECT count(*) FROM vtb.n WHERE v = 2) AS twos; }
step c_head		{ SELECT (SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::vtb.k)) AS k_diff, (SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::vtb.n)) AS n_diff; }

permutation c_reset a_rc b_rc a_write b_write a_commit b_commit c_status c_flush c_full c_rows c_head
permutation c_reset a_rr b_rr a_write b_write a_commit b_commit c_status c_flush c_full c_rows c_head
