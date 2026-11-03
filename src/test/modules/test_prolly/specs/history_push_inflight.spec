# A push's commit is where history moves: a transaction whose snapshot is
# older reads what was pushed still (its bytes kept until no snapshot
# could), even once the bundle is released, and one after reads it no
# more (an error, until fetched back); and a transaction holding a chunk
# as the push frees keeps it (what it holds is not freed).

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE OR REPLACE FUNCTION hp_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(test_engine_chunk(d)), 1, 20)';
	SELECT stored FROM test_cs_put('pages', test_engine_chunk('\x0a2101'));
	SELECT stored FROM test_cs_put('pages', test_engine_chunk('\x0a2102'));
	SELECT test_cs_ref_root('pages', hp_h('\x0a2101'));
	SELECT test_cs_ref_root('pages', hp_h('\x0a2102'));
}

teardown
{
	SELECT count(pg_history_release(name)) FROM pg_history.bundles WHERE name = 'hp';
	SELECT test_cs_free('pages', ARRAY[hp_h('\x0a2102')]);
}

session r
setup			{ SET chunk_free_in_background = off; }
step r_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step r_read		{ SELECT test_engine_unchunk(test_cs_get('pages', hp_h('\x0a2101'))) AS bytes; }
step r_commit	{ COMMIT; }

session w
setup			{ SET chunk_free_in_background = off; }
step w_begin	{ BEGIN; }
step w_hold		{ SELECT test_cs_hold('pages', hp_h('\x0a2102')) AS held; }
step w_commit	{ COMMIT; }

session p
setup			{ SET chunk_free_in_background = off; }
step p_push		{
	SELECT test_cs_unref_root('pages', hp_h('\x0a2101'));
	SELECT test_cs_unref_root('pages', hp_h('\x0a2102'));
	SELECT pg_history_push('hp', ARRAY[hp_h('\x0a2101'), hp_h('\x0a2102')]) AS copied;
}
step p_after	{ SELECT test_engine_unchunk(test_cs_get('pages', hp_h('\x0a2101'))) AS bytes; }
step p_held		{ SELECT test_cs_has('pages', hp_h('\x0a2102')) AS kept; }
step p_release	{ SELECT pg_history_release('hp') AS freed; }

# a reader whose snapshot is older reads on; a new one is refused
permutation r_begin r_read p_push r_read p_after r_commit
# and once the bundle is released too
permutation r_begin r_read p_push p_release r_read p_after r_commit
# a chunk held as the push frees: kept
permutation w_begin w_hold p_push w_commit p_held
