# Version control: a hard reset to the head gives the branch's tables the
# pages kept as of its commit, rather than writing rows back.  A commit
# made while another transaction writes the branch keeps no pages (they
# hold that transaction's rows, which the commit doesn't): the reset after
# gives the tables the pages of the nearest commit before it that kept
# them (kept with no other writer at work), and writes the rows the two
# differ by.  A reset while another transaction holds a table
# (reading it) waits for it, as TRUNCATE does, and then gives the pages
# back; the reader's snapshot reads what it saw meanwhile.  A snapshot
# that didn't hold the table, older than the reset, goes on reading what
# it saw too: the storage the reset replaced, kept for it.

setup		{ CREATE SCHEMA vrk VERSIONED; }
setup
{
	CREATE TABLE vrk.t (id int PRIMARY KEY, v text);
	INSERT INTO vrk.t SELECT g, 'base' FROM generate_series(1, 20) g;
	CREATE TABLE vrk.u (id int PRIMARY KEY);
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vrk base'); END $$; }
setup
{
	CREATE FUNCTION vrk_counted(n text) RETURNS bigint LANGUAGE sql AS
	$$SELECT value FROM pg_stat_prolly WHERE name = n$$;
}

teardown
{
	DO $$
	BEGIN
		PERFORM dolt_reset('--hard');
		DROP SCHEMA vrk CASCADE;
		PERFORM dolt_commit('-Am', 'vrk gone');
	END $$;
	DROP FUNCTION vrk_counted(text);
}

session s1
step s1_dirty	{ UPDATE vrk.t SET v = 'dirty' WHERE id <= 10; }
# (the session's counts flushed as the step's transaction ends)
step s1_flush	{ SELECT pg_stat_force_next_flush(); }
step s1_mark
{
	SELECT set_config('vrk.applied', vrk_counted('apply_rows')::text, false) IS NOT NULL AS marked,
		set_config('vrk.restored', vrk_counted('relations_restored')::text, false) IS NOT NULL AS too;
}
step s1_reset	{ SELECT dolt_reset('--hard') IS NOT NULL AS ok; }
step s1_how
{
	SELECT vrk_counted('apply_rows') > current_setting('vrk.applied')::bigint AS rows_written,
		vrk_counted('relations_restored') > current_setting('vrk.restored')::bigint AS pages_given_back,
		(SELECT count(*) FROM vrk.t WHERE v = 'base') AS base_rows;
}
step s1_commit	{ SELECT dolt_commit('-am', 'vrk commit') IS NOT NULL AS ok; }
step s1_kmark
{
	SELECT set_config('vrk.kept', vrk_counted('pages_kept')::text, false) IS NOT NULL AS marked;
}
step s1_kept
{
	SELECT vrk_counted('pages_kept') = current_setting('vrk.kept')::bigint AS kept_nothing;
}

session s2
step s2_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2_read	{ SELECT count(*) FILTER (WHERE v = 'dirty') AS dirty FROM vrk.t; }
step s2_other	{ SELECT count(*) FROM vrk.u; }
step s2_write	{ INSERT INTO vrk.t VALUES (100, 'in flight'); }
step s2_end		{ COMMIT; }
step s2_gone	{ SELECT count(*) AS in_flight FROM vrk.t WHERE id = 100; }

# nobody else on the branch: the pages given back, no row written
permutation s1_dirty s1_flush s1_mark s1_reset s1_flush s1_how

# a reader holding the table: the reset waits for it, the reader's view as
# it was, and then gives the pages back
permutation s1_dirty s2_begin s2_read s1_flush s1_mark s1_reset s2_read s2_end s1_flush s1_how

# a reader that didn't hold the table, its snapshot older than the reset,
# reads what it saw: the storage the reset replaced
permutation s1_dirty s2_begin s2_other s1_flush s1_mark s1_reset s1_flush s1_how s2_read s2_end

# a commit while another writes keeps nothing: the reset gives back the
# pages kept as of the commit before (the base's, kept with nobody else
# writing) and writes the rows between, and the other's row, committed
# after the commit, is discarded with the working set
permutation s2_begin s2_write s1_dirty s1_flush s1_kmark s1_commit s1_flush s1_kept s2_end s1_dirty s1_flush s1_mark s1_reset s1_flush s1_how s2_gone
