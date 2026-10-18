# Version control, branch-auto writer mode: a session's first write on
# main moves it to its writer branch, forked from main's head as its
# snapshot saw it.  A repeatable read transaction whose snapshot predates a
# newer head fails, to be retried; read committed forks from the new head.

setup		{ CREATE SCHEMA vwb VERSIONED; }
setup
{
	CREATE TABLE vwb.t (id int PRIMARY KEY, v text);
	INSERT INTO vwb.t VALUES (1, 'base');
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vwb base'); END $$; }
setup		{ SELECT dolt_writer_mode('branch-auto') AS mode; }

teardown
{
	DO $$
	BEGIN
		PERFORM dolt_writer_mode('shared');
		DROP SCHEMA vwb CASCADE;
		PERFORM dolt_commit('-Am', 'vwb gone');
	END $$;
}

session a
setup			{ SET dolt.writer = 'ann'; SET client_min_messages = warning; }
step a_rr		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step a_rc		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step a_read		{ SELECT count(*) FROM vwb.t; }
step a_insert	{ INSERT INTO vwb.t VALUES (3, 'ann'); }
step a_rows		{ SELECT string_agg(id || ':' || v, ',' ORDER BY id) AS rows, active_branch() FROM vwb.t; }
step a_commit	{ COMMIT; }
step a_rollback	{ ROLLBACK; }
step a_where	{ SELECT active_branch(); }
step a_main		{ SELECT (dolt_checkout('main'))[1] = '0' AS ok; }
step a_drop		{ SELECT dolt_branch('-D', 'writer/ann/main'); }

session b
setup			{ SET dolt.writer = 'bea'; SET client_min_messages = warning; }
step b_write	{ INSERT INTO vwb.t VALUES (2, 'bea'); }
step b_integrate { SELECT (dolt_integrate('-m', 'bea'))[2] AS fast_forward; }
step b_main		{ SELECT (dolt_checkout('main'))[1] = '0' AS ok; }

# main moved since ann's snapshot: her repeatable read write fails, and
# leaves her on main
permutation a_rr a_read b_write b_integrate b_main a_insert a_rollback a_where

# read committed: forked from main's new head, which has bea's row
permutation a_rc a_read b_write b_integrate b_main a_insert a_rows a_commit a_main a_drop
