# A pack put while another transaction is putting one of its chunks: the
# pack's put waits for that transaction to end, as a put of the chunk does,
# then has it there (committed: the bytes the pack appended for it are
# dead, its other chunks put) or puts it (aborted).

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
}

session a
setup		{ SET chunk_free_in_background = off; }
step a_begin	{ BEGIN; }
step a_put		{ SELECT stored FROM test_cs_put('pages', '\x0a0101'); }
step a_commit	{ COMMIT; }
step a_rollback	{ ROLLBACK; }

session b
setup		{
	SET chunk_free_in_background = off;
	DROP TABLE IF EXISTS dead0;
	CREATE TEMP TABLE dead0 AS SELECT dead FROM test_chunk_log_space('pg_history.chunks');
}
step b_pack		{
	SELECT test_cs_put_many('pages',
		ARRAY[substr(sha256('\x0a0101'), 1, 20), substr(sha256('\x0a0102'), 1, 20)],
		ARRAY['\x0a0101', '\x0a0102']::bytea[]) AS stored;
}
step b_has		{
	SELECT test_cs_has('pages', substr(sha256('\x0a0101'), 1, 20)) AS first,
		test_cs_has('pages', substr(sha256('\x0a0102'), 1, 20)) AS second;
}
step b_dead		{
	SELECT l.dead - d.dead AS more_dead FROM test_chunk_log_space('pg_history.chunks') l, dead0 d;
}
step b_free		{
	SELECT test_cs_free('pages', ARRAY[substr(sha256('\x0a0101'), 1, 20),
		substr(sha256('\x0a0102'), 1, 20)]) AS freed;
}

# the chunk another is putting: the pack waits, then has it, its own bytes
# for it dead, and puts the other
permutation a_begin a_put b_pack a_commit b_has b_dead b_free
# or, if it aborts, puts both
permutation a_begin a_put b_pack a_rollback b_has b_free
