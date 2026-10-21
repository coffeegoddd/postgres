# A repeatable read transaction whose snapshot is older than another
# session's fold folds from that fold's baseline all the same: what the
# baseline holds of transactions it doesn't see finished is undone (keys
# re-read by its snapshot, a keyless table's counts taken back), and its
# dolt_status, dolt_diff and verified folds match a fold of every row,
# reading no table in full; the next fold, in a fresh transaction, is from
# the baseline too.  The other transaction committed before the fold, in
# flight at the snapshot, or aborted; a key written before the snapshot and
# again after; the stale transaction's own writes.

setup		{ CREATE SCHEMA vsf VERSIONED; }
setup
{
	CREATE TABLE vsf.k (id int PRIMARY KEY, v text);
	CREATE TABLE vsf.n (v int);
	INSERT INTO vsf.k SELECT g, 'k' FROM generate_series(1, 100) g;
	INSERT INTO vsf.n SELECT g % 5 FROM generate_series(1, 100) g;
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vsf base'); END $$; }

teardown
{
	DO $$
	BEGIN
		PERFORM dolt_reset('--hard', 'HEAD');
		DROP SCHEMA vsf CASCADE;
		PERFORM dolt_commit('-Am', 'vsf gone');
	END $$;
}

session a
step a_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) AS k_rows FROM vsf.k; }
step a_write	{ UPDATE vsf.k SET v = 'a' WHERE id = 50; INSERT INTO vsf.n VALUES (4); }
step a_status	{ SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2; }
step a_diff		{ SELECT (SELECT string_agg(diff_type || ':' || coalesce((to_row).id, (from_row).id) || ':' || coalesce((to_row).v, '-'), ',' ORDER BY coalesce((to_row).id, (from_row).id)) FROM dolt_diff('HEAD', 'WORKING', NULL::vsf.k)) AS k_diff, (SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::vsf.n)) AS n_diff; }
step a_verify	{ SELECT table_name, matches FROM dolt_verify_folds() ORDER BY 1; }
step a_commit	{ COMMIT; }
step a_flush	{ SELECT pg_stat_force_next_flush(); }

session b
step b_begin	{ BEGIN; }
step b_write	{ UPDATE vsf.k SET v = 'b' WHERE id = 1; INSERT INTO vsf.n VALUES (1); DELETE FROM vsf.n WHERE ctid = (SELECT min(ctid) FROM vsf.n WHERE v = 2); }
step b_write2	{ UPDATE vsf.k SET v = 'b2' WHERE id = 1; INSERT INTO vsf.k VALUES (200, 'new'); DELETE FROM vsf.k WHERE id = 7; INSERT INTO vsf.n VALUES (3), (3); }
step b_commit	{ COMMIT; }
step b_abort	{ ROLLBACK; }

session c
step c_reset	{ SELECT pg_stat_reset_shared('prolly'); }
step c_fold		{ SELECT (dolt_commit('-am', 'c commits'))[1] IS NOT NULL AS committed; }
step c_full		{ SELECT sum(value) FILTER (WHERE name = 'fold_full_tables') - sum(value) FILTER (WHERE name = 'fold_full_verify') AS full_folds, sum(value) FILTER (WHERE name IN ('fold_full_stale_snapshot', 'fold_full_unsettled')) AS stale_full FROM pg_stat_prolly; }
step c_status	{ SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 2; }
step c_verify	{ SELECT table_name, matches FROM dolt_verify_folds() ORDER BY 1; }
step c_flush	{ SELECT pg_stat_force_next_flush(); }

# another transaction's writes, committed after the snapshot, folded
permutation a_begin b_begin b_write b_commit c_fold c_reset a_status a_diff a_verify a_commit a_flush c_full c_status c_verify c_flush c_full
# in flight at the snapshot, committed and folded after
permutation b_begin b_write a_begin b_commit c_fold c_reset a_status a_diff a_verify a_commit a_flush c_full c_status c_verify c_flush c_full
# a key written before the snapshot, then again after, and folded
permutation b_begin b_write b_commit a_begin b_begin b_write2 b_commit c_fold c_reset a_status a_diff a_verify a_commit a_flush c_full c_status c_verify c_flush c_full
# aborted: nothing of it held, nothing undone
permutation b_begin b_write a_begin b_abort c_fold c_reset a_status a_diff a_verify a_commit a_flush c_full c_status c_verify c_flush c_full
# the stale transaction's own writes
permutation a_begin b_begin b_write b_commit c_fold c_reset a_write a_status a_diff a_verify a_commit a_flush c_full c_status c_verify c_flush c_full
