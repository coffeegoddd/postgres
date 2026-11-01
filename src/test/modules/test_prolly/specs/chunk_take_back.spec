# A put that finds another put its chunk first takes back the references
# it counted: a decrease of no more than its own increase, which waits for
# no other transaction's decrease (no other sees what it takes back).

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE OR REPLACE FUNCTION tb_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	-- (a chunk of the test format referencing the leaf '\x0a0601')
	CREATE OR REPLACE FUNCTION tb_parent() RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT '\x5201'::bytea || tb_h('\x0a0601')$$;
	SELECT stored FROM test_cs_put('pages', '\x0a0601');
	SELECT test_cs_ref_root('pages', tb_h('\x0a0601'));
}

teardown
{
	SELECT test_chunk_format_refs(true);
	SELECT test_cs_unref_root('pages', tb_h('\x0a0601'));
	SELECT test_cs_free('pages', ARRAY[tb_h(tb_parent()), tb_h('\x0a0601')]);
	SELECT test_chunk_format_refs(false);
}

session a
setup		{ SET chunk_free_in_background = off; SELECT test_chunk_format_refs(true); }
step a_begin	{ BEGIN; }
step a_put		{ SELECT stored FROM test_cs_put('pages', tb_parent()); }
step a_commit	{ COMMIT; }

session u
setup		{ SET chunk_free_in_background = off; }
step u_begin	{ BEGIN; }
step u_unref	{ SELECT test_cs_unref_root('pages', tb_h('\x0a0601')); }
step u_rollback	{ ROLLBACK; }

session t
setup		{ SET chunk_free_in_background = off; SELECT test_chunk_format_refs(true); }
step t_put		{ SELECT stored FROM test_cs_put('pages', tb_parent()); }
step t_counts	{ SELECT refs, roots FROM test_cs_counts('pages', tb_h('\x0a0601')); }

# t's put waits for a's, finds the chunk a's, and takes back its reference
# without waiting for u's decrease of the leaf's roots
permutation a_begin a_put u_begin u_unref t_put a_commit u_rollback t_counts
