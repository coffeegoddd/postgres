# Two transactions freeing chunks at once, each letting go of the
# references of the chunks it frees in an order of its own: a reference is
# let go of only by the freeing of the chunk it is from, which one alone
# may free, so neither waits for the other's (each waiting for the other,
# they would deadlock).

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE OR REPLACE FUNCTION fo_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	-- (chunks of the test format: '\x52', the count, the chunks named)
	CREATE OR REPLACE FUNCTION fo_parent(tag bytea, kid bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT '\x5202'::bytea || fo_h(kid) || fo_h(tag)$$;
	SELECT test_chunk_format_refs(true);
	SELECT stored FROM test_cs_put('pages', '\x0a0c01');
	SELECT stored FROM test_cs_put('pages', '\x0a0c02');
	SELECT stored FROM test_cs_put('pages', '\x0a0cf1');
	SELECT stored FROM test_cs_put('pages', '\x0a0cf2');
	SELECT stored FROM test_cs_put('pages', '\x0a0cf3');
	SELECT stored FROM test_cs_put('pages', '\x0a0cf4');
	-- two parents of each of two leaves (each held by both)
	SELECT stored FROM test_cs_put('pages', fo_parent('\x0a0cf1', '\x0a0c01'));
	SELECT stored FROM test_cs_put('pages', fo_parent('\x0a0cf2', '\x0a0c02'));
	SELECT stored FROM test_cs_put('pages', fo_parent('\x0a0cf3', '\x0a0c02'));
	SELECT stored FROM test_cs_put('pages', fo_parent('\x0a0cf4', '\x0a0c01'));
}

teardown
{
	SELECT test_chunk_format_refs(true);
	SELECT test_cs_free_unheld('pages');
}

session a
setup		{ SET chunk_free_in_background = off; SELECT test_chunk_format_refs(true); }
step a_begin	{ BEGIN; }
step a_free1	{ SELECT test_cs_free('pages', ARRAY[fo_h(fo_parent('\x0a0cf1', '\x0a0c01'))]) AS freed; }
step a_free2	{ SELECT test_cs_free('pages', ARRAY[fo_h(fo_parent('\x0a0cf2', '\x0a0c02'))]) AS freed; }
step a_commit	{ COMMIT; }

session b
setup		{ SET chunk_free_in_background = off; SELECT test_chunk_format_refs(true); }
step b_begin	{ BEGIN; }
step b_free1	{ SELECT test_cs_free('pages', ARRAY[fo_h(fo_parent('\x0a0cf3', '\x0a0c02'))]) AS freed; }
step b_free2	{ SELECT test_cs_free('pages', ARRAY[fo_h(fo_parent('\x0a0cf4', '\x0a0c01'))]) AS freed; }
step b_commit	{ COMMIT; }

session c
setup		{ SELECT test_chunk_format_refs(true); }
step c_counts	{
	SELECT string_agg(coalesce(refs::text, '-'), ',' ORDER BY k) AS refs
	FROM unnest(ARRAY['\x0a0c01', '\x0a0c02']::bytea[]) WITH ORDINALITY u(d, k),
		LATERAL test_cs_counts('pages', fo_h(d));
}

# a lets go of a reference of leaf 1, b of leaf 2; then a of leaf 2, b of
# leaf 1: neither waits, both commit, and both leaves have none left
permutation a_begin b_begin a_free1 b_free1 a_free2 b_free2 a_commit b_commit c_counts
