# Version control, owned writer mode: a branch dirty with one writer's
# committed changes refuses another at once; a claim waits for a writer
# whose transaction is still in flight, then looks again; dolt.writer_nowait
# and lock_timeout bound the wait; two sessions of one writer write the
# branch together, and a commit by one leaves the owner while the other is
# still writing.

setup		{ CREATE SCHEMA vwc VERSIONED; }
setup
{
	CREATE TABLE vwc.t (id int PRIMARY KEY, v text);
	INSERT INTO vwc.t VALUES (1, 'base');
}
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vwc base'); END $$; }
setup		{ SELECT dolt_writer_mode('owned') AS mode; }

teardown
{
	DO $$
	BEGIN
		PERFORM dolt_writer_mode('shared');
		PERFORM dolt_reset('--hard');
		DROP SCHEMA vwc CASCADE;
		PERFORM dolt_commit('-Am', 'vwc gone');
	END $$;
}

session a
setup			{ SET dolt.writer = 'alice'; }
step a_begin	{ BEGIN; }
step a_insert	{ INSERT INTO vwc.t VALUES (2, 'alice'); }
step a_commit	{ COMMIT; }
step a_rollback	{ ROLLBACK; }
step a_sleep		{ SELECT pg_sleep(0.5); }
step a_dolt		{ SELECT length(dolt_commit('-m', 'alice')::text) AS committed; }
step a_owner	{ SELECT owner FROM dolt_branches() WHERE name = 'main'; }

session a2
setup			{ SET dolt.writer = 'alice'; }
step a2_begin	{ BEGIN; }
step a2_insert	{ INSERT INTO vwc.t VALUES (3, 'alice too'); }
step a2_commit	{ COMMIT; }

session b
setup			{ SET dolt.writer = 'bob'; RESET dolt.writer_nowait; RESET lock_timeout; }
step b_insert	{ INSERT INTO vwc.t VALUES (4, 'bob'); }
step b_nowait	{ SET dolt.writer_nowait = on; }
step b_timeout	{ SET lock_timeout = '100ms'; }
step b_reset	{ SELECT dolt_reset('--hard') AS reset; }

# committed changes: refused at once
permutation a_insert b_insert a_dolt b_insert a_owner b_reset

# a claim in flight: bob waits, then is refused once alice's commits
permutation a_begin a_insert b_insert a_commit a_owner a_dolt

# ...or claims once alice's rolls back
permutation a_begin a_insert b_insert a_rollback a_owner b_reset

# bounded waits
permutation a_begin a_insert b_nowait b_insert a_rollback
permutation a_begin a_insert b_timeout b_insert a_sleep a_rollback

# one writer, two sessions: both write; a commit while the other writes
# keeps the owner, the next one clears it
permutation a_insert a2_begin a2_insert a_dolt a_owner a2_commit a_dolt a_owner
