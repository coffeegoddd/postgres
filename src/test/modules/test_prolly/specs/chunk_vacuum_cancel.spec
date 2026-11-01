# VACUUM of a chunk index waits for its log's lock before it takes a
# bucket's entries out and counts their bytes dead with interrupts held: a
# VACUUM waiting for the lock is cancelled.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE TABLE vcl () USING prolly_chunks;
	CREATE TABLE vci () USING prolly_chunk_index;
	SELECT test_chunk_put('vcl', 'vci', substr(sha256('\x01'), 1, 20), '\x01');
}

teardown
{
	DROP TABLE vcl, vci;
}

session a
setup		{ SET chunk_free_in_background = off; }
step a_abort	{
	BEGIN;
	SELECT test_chunk_put('vcl', 'vci', substr(sha256('\x02'), 1, 20), '\x02');
	ROLLBACK;
}
step a_lock		{ BEGIN; LOCK TABLE vcl IN ACCESS EXCLUSIVE MODE; }
step a_commit	{ COMMIT; }

session v
setup		{ SET chunk_free_in_background = off; SET application_name = 'vacuuming'; }
step v_vacuum	{ VACUUM vci; }

session c
step c_cancel	{
	SELECT pg_cancel_backend(pid) FROM pg_stat_activity
	WHERE application_name = 'vacuuming' AND wait_event_type = 'Lock';
}

permutation a_abort a_lock v_vacuum c_cancel a_commit
