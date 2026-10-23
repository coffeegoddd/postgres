# A repeatable read transaction whose snapshot is older than another
# session's fold, folding from that fold's baseline with more changes to
# undo than maintenance_work_mem holds: they are sorted, spilling, and
# undone a part at a time, keys re-read by its snapshot and a keyless
# table's counts taken back, with no table read in full.  Its dolt_status
# and verified folds match a fold of every row.

setup		{ CREATE SCHEMA vss VERSIONED; }
setup
{
	CREATE TABLE vss.k (id int PRIMARY KEY, v text);
	CREATE TABLE vss.n (v int, w text);
	INSERT INTO vss.k SELECT g, 'k' FROM generate_series(1, 5000) g;
	INSERT INTO vss.n SELECT g % 50, 'n' FROM generate_series(1, 5000) g;
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vss base'); END $$; }

teardown
{
	DO $$
	BEGIN
		PERFORM dolt_reset('--hard', 'HEAD');
		DROP SCHEMA vss CASCADE;
		PERFORM dolt_commit('-Am', 'vss gone');
	END $$;
}

session a
setup		{ SET maintenance_work_mem = '64kB'; }
step a_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) AS k_rows FROM vss.k; }
step a_status	{ SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2; }
step a_diff		{ SELECT (SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::vss.k)) AS k_diff, (SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::vss.n)) AS n_diff; }
step a_verify	{ SELECT table_name, matches FROM dolt_verify_folds() ORDER BY 1; }
step a_commit	{ COMMIT; }
step a_flush	{ SELECT pg_stat_force_next_flush(); }

session b
step b_write	{ UPDATE vss.k SET v = repeat('b', 200) WHERE id % 2 = 0; DELETE FROM vss.k WHERE id % 7 = 1; INSERT INTO vss.n SELECT g % 3000, repeat('w', 200) FROM generate_series(1, 3000) g; DELETE FROM vss.n WHERE v = 3; }

session c
step c_reset	{ SELECT pg_stat_reset_shared('prolly'); }
step c_fold		{ SELECT (dolt_commit('-am', 'c commits'))[1] IS NOT NULL AS committed; }
step c_full		{ SELECT sum(value) FILTER (WHERE name = 'fold_full_tables') - sum(value) FILTER (WHERE name = 'fold_full_verify') AS full_folds, sum(value) FILTER (WHERE name = 'fold_changes_spilled') > 0 AS spilled FROM pg_stat_prolly; }

# another transaction's writes, committed after the snapshot, folded by
# another session: the stale fold undoes them, sorted
permutation a_begin b_write c_fold c_reset a_status a_diff a_verify a_commit a_flush c_full
