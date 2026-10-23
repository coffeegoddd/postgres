# A session's fold after its last, working out again only the tables
# written since, finds those another session wrote and committed in
# between, told of by the refs as each writer ends; while another is
# still writing, it folds everything.  A catalog change
# another session commits folds everything.  Each of session a's folds is
# checked against one of everything (debug_prolly_verify_fold_caches).

setup		{ CREATE SCHEMA vic VERSIONED; }
setup
{
	CREATE TABLE vic.a (id int PRIMARY KEY, v text);
	CREATE TABLE vic.b (id int PRIMARY KEY, v text);
	CREATE TABLE vic.c (id int PRIMARY KEY, v text);
	INSERT INTO vic.a VALUES (1, 'a');
	INSERT INTO vic.b VALUES (1, 'b');
	INSERT INTO vic.c VALUES (1, 'c');
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vic base'); END $$; }

teardown
{
	DO $$
	BEGIN
		PERFORM dolt_reset('--hard', 'HEAD');
		DROP SCHEMA vic CASCADE;
		PERFORM dolt_commit('-Am', 'vic gone');
	END $$;
}

session a
setup		{ SET debug_prolly_verify_fold_caches = on; }
step a_write	{ UPDATE vic.a SET v = v || '.'; }
step a_commit	{ SELECT (dolt_commit('-am', 'a commits'))[1] IS NOT NULL AS committed; }
step a_flush	{ SELECT pg_stat_force_next_flush(); }
step a_diff		{ SELECT coalesce(to_table_name, from_table_name) AS table_name, diff_type, schema_change FROM dolt_diff_summary('HEAD~1', 'HEAD') ORDER BY 1; }
step a_count	{ SELECT sum(value) FILTER (WHERE name = 'fold_incremental') > 0 AS incremental FROM pg_stat_prolly; }
step a_reset	{ SELECT pg_stat_reset_shared('prolly'); }

session b
step b_write	{ UPDATE vic.b SET v = v || '!'; }
step b_begin	{ BEGIN; UPDATE vic.c SET v = 'in flight'; }
step b_commit	{ COMMIT; }
step b_alter	{ ALTER TABLE vic.c ADD COLUMN w int; }

# another session's committed write between two of a's commits: in the
# second, which works out again a's table and b's
permutation a_write a_commit a_write b_write a_reset a_commit a_flush a_count a_diff
# a write in flight as a commits: everything folded (a writer is seen
# committed before the refs hear what it wrote), its rows not in it (not
# seen); in the next
permutation a_write a_commit a_write b_begin a_reset a_commit a_flush a_count a_diff b_commit a_write a_commit a_diff
# a catalog change another session commits: everything folded (the
# commit's later folds from that one)
permutation a_write a_commit b_alter a_write a_commit a_diff
