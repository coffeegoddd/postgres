# Version control, dolt.transaction_commit: with every writer in the mode,
# each committed transaction makes one commit, in commit order, holding its
# own rows, on the head as it is when it commits: at read committed and at
# repeatable read alike (a snapshot older than the head is no failure; the
# heap's row locks have decided what commits together).

setup		{ CREATE SCHEMA vtc VERSIONED; }
setup		{ CREATE TABLE vtc.t (id int PRIMARY KEY, v text); }
setup		{ DO $$ BEGIN PERFORM dolt_commit('-Am', 'vtc base'); END $$; }

teardown
{
	DO $$
	BEGIN
		DROP SCHEMA vtc CASCADE;
		PERFORM dolt_commit('-Am', 'vtc gone');
	END $$;
}

session a
setup			{ SET dolt.transaction_commit = on; SET dolt.transaction_commit_message = 'a'; }
step a_rc		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step a_rr		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step a_read		{ SELECT count(*) FROM vtc.t; }
step a_insert	{ INSERT INTO vtc.t VALUES (1, 'a'); }
step a_commit	{ COMMIT; }

session b
setup			{ SET dolt.transaction_commit = on; SET dolt.transaction_commit_message = 'b'; }
step b_begin	{ BEGIN; }
step b_insert	{ INSERT INTO vtc.t VALUES (2, 'b'); }
step b_commit	{ COMMIT; }

session c
step c_log
{
	SELECT string_agg(message, ',' ORDER BY commit_order DESC) AS log
	  FROM dolt_log('main') WHERE message IN ('a', 'b');
}
step c_heads
{
	SELECT (SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM vtc.t) AS rows,
		   (SELECT dirty FROM dolt_branches() WHERE name = 'main') AS dirty;
}
step c_reset	{ DELETE FROM vtc.t; SELECT length(dolt_commit('-Am', 'vtc emptied')::text) AS committed; }

# read committed: b commits first, then a, each one commit
permutation a_rc a_insert b_begin b_insert b_commit a_commit c_log c_heads c_reset

# repeatable read: a's snapshot predates b's head; a commits on it anyway
permutation a_rr a_read b_begin b_insert b_commit a_insert a_commit c_log c_heads c_reset
