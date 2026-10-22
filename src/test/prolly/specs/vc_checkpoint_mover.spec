# A checkpoint moves a branch's baseline (its fold consumes the change
# log) without moving its head.  A READ COMMITTED commit that took its
# snapshot before another session's checkpoint, and waited for the refs
# behind it, folds by a snapshot taken once it has them: incrementally,
# from the checkpoint's baseline, not every table in full (which would
# leave each baseline unsettled, for the next commit to read in full
# again).  A session leaving the branch so leaves the checkpoint's
# baseline: leaving takes no checkpoint where the change finder keeps
# nothing for one (the log's, #656), so it doesn't wait for the refs; one
# by its older snapshot would read every table in full and replace the
# checkpoint's baseline with an unsettled one.

setup		{ CREATE SCHEMA vcm VERSIONED; }
setup
{
	CREATE TABLE vcm.t1 (id int PRIMARY KEY, v text);
	CREATE TABLE vcm.t2 (id int PRIMARY KEY, v text);
	INSERT INTO vcm.t1 SELECT g, 'one' FROM generate_series(1, 200) g;
	INSERT INTO vcm.t2 SELECT g, 'two' FROM generate_series(1, 200) g;
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vcm base'); END $$; }
setup
{
	DO $$
	BEGIN
		PERFORM dolt_branch('vcm_other');
		PERFORM dolt_checkout('vcm_other');
		PERFORM dolt_checkout('main');
	END $$;
}
# (the setup's own folds counted before a session resets the counters)
setup		{ SELECT pg_stat_force_next_flush(); }

teardown
{
	DO $$
	BEGIN
		PERFORM dolt_reset('--hard', 'HEAD');
		DROP SCHEMA vcm CASCADE;
		PERFORM dolt_commit('-Am', 'vcm gone');
		PERFORM dolt_branch('-D', 'vcm_other');
	END $$;
}

session a
step a_write	{ UPDATE vcm.t2 SET v = 'a' WHERE id = 1; }
step a_reset	{ SELECT pg_stat_reset_shared('prolly'); }
step a_dcommit	{ SELECT (dolt_commit('-am', 'a commits'))[1] IS NOT NULL AS committed; }
step a_write2	{ UPDATE vcm.t2 SET v = 'a2' WHERE id = 2; }
step a_dcommit2	{ SELECT (dolt_commit('-am', 'a again'))[1] IS NOT NULL AS committed; }
step a_leave	{ SELECT (dolt_checkout('vcm_other'))[1] AS status; }
step a_back		{ SELECT (dolt_checkout('main'))[1] AS status; }
step a_flush	{ SELECT pg_stat_force_next_flush(); }
step a_full		{ SELECT value AS full_folds FROM pg_stat_prolly WHERE name = 'fold_full_tables'; }

session b
step b_begin	{ BEGIN; }
step b_write	{ UPDATE vcm.t1 SET v = 'b' WHERE id = 1; }
step b_ckpt		{ SELECT prolly_checkpoint() IS NOT NULL AS checkpointed; }
step b_commit	{ COMMIT; }
step b_flush	{ SELECT pg_stat_force_next_flush(); }

# b checkpoints, holding the refs; a's commit waits for them, its snapshot
# taken before b's commit; then a commits again
permutation a_write b_begin b_write b_ckpt a_reset a_dcommit b_commit b_flush a_write2 a_dcommit2 a_flush a_full

# b checkpoints, holding the refs; a leaves its dirty main, its snapshot
# taken before b's commit, and takes no checkpoint, so it doesn't wait for
# them; then a comes back and commits, folding from b's baseline
permutation a_write b_begin b_write b_ckpt a_reset a_leave b_commit b_flush a_back a_write2 a_dcommit2 a_flush a_full
