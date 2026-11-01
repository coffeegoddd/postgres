# A pack put while another transaction is putting one of its chunks: the
# pack's put waits for that transaction to end, as a put of the chunk does,
# then has it there (committed: the pack reserved and wrote no bytes for
# it, none dead; its other chunks put) or puts it (aborted).  A chunk of
# the pack referencing it counts its reference either way.  A pack's put
# cancelled while it waits wrote none of its bytes: those reserved for the
# chunks it had put, their entries aborted, VACUUM counts dead.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE OR REPLACE FUNCTION pack_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	-- (a chunk of the test format referencing the leaf '\x0a0201')
	CREATE OR REPLACE FUNCTION pack_parent() RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT '\x5201'::bytea || pack_h('\x0a0201')$$;
}

session a
setup		{ SET chunk_free_in_background = off; }
step a_begin	{ BEGIN; }
step a_put		{ SELECT stored FROM test_cs_put('pages', '\x0a0101'); }
step a_put_leaf	{ SELECT stored FROM test_cs_put('pages', '\x0a0201'); }
step a_commit	{ COMMIT; }
step a_rollback	{ ROLLBACK; }

session b
setup		{
	SET chunk_free_in_background = off;
	SET application_name = 'pack_putter';
}
# (each permutation from a store VACUUM has seen, its dead bytes noted)
step b_start	{ VACUUM pg_history.chunk_index; }
step b_mark		{
	DROP TABLE IF EXISTS dead0;
	CREATE TEMP TABLE dead0 AS SELECT dead FROM test_chunk_log_space('pg_history.chunks');
}
step b_pack		{
	SELECT test_cs_put_many('pages',
		ARRAY[pack_h('\x0a0101'), pack_h('\x0a0102')],
		ARRAY['\x0a0101', '\x0a0102']::bytea[]) AS stored;
}
step b_has		{
	SELECT test_cs_has('pages', pack_h('\x0a0101')) AS first,
		test_cs_has('pages', pack_h('\x0a0102')) AS second;
}
step b_dead		{
	SELECT l.dead - d.dead AS more_dead FROM test_chunk_log_space('pg_history.chunks') l, dead0 d;
}
step b_vacuum	{ VACUUM pg_history.chunk_index; }
# (a chunk after the pack's, keeping the log's tail, which VACUUM would cut)
step b_live		{ SELECT stored FROM test_cs_put('pages', '\x0a0109'); }
step b_free_live	{ SELECT test_cs_free('pages', ARRAY[pack_h('\x0a0109')]) AS freed; }
step b_free		{
	SELECT test_cs_free('pages', ARRAY[pack_h('\x0a0101'), pack_h('\x0a0102')]) AS freed;
}
step b_parent	{
	SELECT test_chunk_format_refs(true);
	SELECT test_cs_put_many('pages', ARRAY[pack_h(pack_parent()), pack_h('\x0a0201')],
		ARRAY[pack_parent(), '\x0a0201']) AS stored;
}
step b_leaf		{
	SELECT refs, roots FROM test_cs_counts('pages', pack_h('\x0a0201'));
}
step b_free_parent	{
	SELECT test_cs_free('pages', ARRAY[pack_h(pack_parent())]) AS freed;
	SELECT test_chunk_format_refs(false);
}

session c
step c_cancel	{
	SELECT pg_cancel_backend(pid) FROM pg_stat_activity
	WHERE application_name = 'pack_putter' AND wait_event_type = 'Lock';
}

# the chunk another is putting: the pack waits, then has it, its own bytes
# for it dead, and puts the other
permutation b_start b_mark a_begin a_put b_pack a_commit b_has b_dead b_free
# or, if it aborts, puts both
permutation b_start b_mark a_begin a_put b_pack a_rollback b_has b_free
# a chunk of the pack referencing the one it waited for: one reference,
# whether that one is the other's put or the pack's
permutation b_start b_mark a_begin a_put_leaf b_parent a_commit b_leaf b_free_parent
permutation b_start b_mark a_begin a_put_leaf b_parent a_rollback b_leaf b_free_parent
# cancelled while it waits (for the second of its chunks by name, the first,
# '\x0a0102', put): nothing written, nothing dead; once VACUUM has the
# entries out, the bytes reserved for the one it put (3) dead, and those
# the other's put, rolled back, wrote (3); neither there
permutation b_start b_mark a_begin a_put b_pack c_cancel b_dead a_rollback b_live b_vacuum b_dead b_has b_free_live
