# Version control: under dolt.transaction_commit a commit's roots are
# captured while other transactions write the branch, holding their
# versions too, and kept unfrozen with the commit's snapshot.  Such a root
# is never given back as it was captured: a hard reset to the commit
# freezes it first, as of that snapshot, so a row another transaction
# wrote before the capture and committed after it (its own commit the
# next one) is not in the commit's rows after the reset.

setup		{ CREATE SCHEMA vlr VERSIONED; }
setup
{
	CREATE TABLE vlr.t (id int PRIMARY KEY, v text NOT NULL);
	CREATE INDEX t_v ON vlr.t (v);
	INSERT INTO vlr.t SELECT g, 'base' FROM generate_series(1, 200) g;
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vlr base'); END $$; }
setup
{
	CREATE FUNCTION vlr_counted(n text) RETURNS bigint LANGUAGE sql AS
	$$SELECT value FROM pg_stat_prolly WHERE name = n$$;
}

teardown
{
	DO $$
	BEGIN
		PERFORM dolt_reset('--hard');
		DROP SCHEMA vlr CASCADE;
		PERFORM dolt_commit('-Am', 'vlr gone');
	END $$;
	DROP FUNCTION vlr_counted(text);
}

session s1
setup
{
	SET prolly.root_interval = 0;
	SET dolt.transaction_commit = on;
}
step s1_flush	{ SELECT pg_stat_force_next_flush(); }
step s1_mark
{
	SELECT set_config('vlr.kept', vlr_counted('pages_kept')::text, false) IS NOT NULL AS marked,
		set_config('vlr.frozen', vlr_counted('roots_frozen_later')::text, false) IS NOT NULL AS too,
		set_config('vlr.restored', vlr_counted('relations_restored')::text, false) IS NOT NULL AS also;
}
# (a commit of its own, its roots captured while s2 writes)
step s1_write	{ UPDATE vlr.t SET v = 'c1' WHERE id = 1; }
step s1_kept
{
	SELECT vlr_counted('pages_kept') > current_setting('vlr.kept')::bigint AS kept;
}
step s1_reset	{ SELECT dolt_reset('--hard', 'HEAD~1') IS NOT NULL AS ok; }
step s1_how
{
	SELECT vlr_counted('roots_frozen_later') > current_setting('vlr.frozen')::bigint AS frozen_then,
		vlr_counted('relations_restored') > current_setting('vlr.restored')::bigint AS pages_given_back,
		(SELECT count(*) FROM vlr.t WHERE id = 1000) AS late_row,
		(SELECT v FROM vlr.t WHERE id = 1) AS c1_row,
		(SELECT count(*) FROM vlr.t) AS rows_now;
}
step s1_index	{ SET enable_seqscan = off; SELECT count(*) AS by_index FROM vlr.t WHERE v = 'late'; }

session s2
setup			{ SET dolt.transaction_commit = on; }
step s2_begin	{ BEGIN; }
step s2_write	{ INSERT INTO vlr.t VALUES (1000, 'late'); }
step s2_end		{ COMMIT; }

# s2's row, on the pages s1's commit captured, committed after it: not in
# s1's commit, nor in the table after the reset to it
permutation s1_flush s1_mark s2_begin s2_write s1_write s1_flush s1_kept s2_end s1_reset s1_flush s1_how s1_index
