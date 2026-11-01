# A pack of many new chunks, its transaction open, holds them with no pins
# (no other backend's freer frees chunks whose entries are not committed,
# and its own passes them by): its table of pins does not fill, which would
# count as pinning every chunk, and another's freeing goes on.  Its own
# freeing keeps them.  So too a pack whose chunks reference many the store
# has (their references, counted, hold them), one put again in the same
# transaction (its chunks found its own), and one whose chunks are all the
# store's already, another's (each held by a change adding nothing to its
# counts, not a pin in a table of fixed size: another's freeing of other
# chunks goes on, and of these waits for nothing and refuses them).

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE OR REPLACE FUNCTION pack_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
}

session p
setup		{ SET chunk_free_in_background = off; }
step p_begin	{ BEGIN; }
step p_pack		{
	SELECT test_cs_put_many('pages',
		ARRAY(SELECT pack_h('\x50'::bytea || int4send(i)) FROM generate_series(1, 3000) i ORDER BY i),
		ARRAY(SELECT '\x50'::bytea || int4send(i) FROM generate_series(1, 3000) i ORDER BY i)) AS stored;
}
step p_own_free	{
	SELECT test_cs_free('pages', ARRAY(SELECT pack_h('\x50'::bytea || int4send(i))
		FROM generate_series(1, 3000) i)) AS freed_own;
}
step p_pack_again	{
	SELECT test_cs_put_many('pages',
		ARRAY(SELECT pack_h('\x50'::bytea || int4send(i)) FROM generate_series(1, 3000) i ORDER BY i),
		ARRAY(SELECT '\x50'::bytea || int4send(i) FROM generate_series(1, 3000) i ORDER BY i)) AS stored_again;
}
step p_parents	{
	SELECT test_chunk_format_refs(true);
	SELECT test_cs_put_many('pages',
		ARRAY(SELECT pack_h('\x5201'::bytea || pack_h('\x50'::bytea || int4send(i)))
			FROM generate_series(1, 3000) i ORDER BY i),
		ARRAY(SELECT '\x5201'::bytea || pack_h('\x50'::bytea || int4send(i))
			FROM generate_series(1, 3000) i ORDER BY i)) AS parents;
}
step p_free_parents	{
	SELECT test_cs_free('pages', ARRAY(SELECT pack_h('\x5201'::bytea || pack_h('\x50'::bytea || int4send(i)))
		FROM generate_series(1, 3000) i)) AS freed;
	SELECT test_chunk_format_refs(false);
}
step p_commit	{ COMMIT; }
step p_free		{
	SELECT test_cs_free('pages', ARRAY(SELECT pack_h('\x50'::bytea || int4send(i))
		FROM generate_series(1, 3000) i)) AS freed;
}

session o
setup		{ SET chunk_free_in_background = off; }
step o_put		{ SELECT stored FROM test_cs_put('pages', '\x0a0501'); }
step o_free		{ SELECT test_cs_free('pages', ARRAY[pack_h('\x0a0501')]) AS freed; }
step o_free_found	{
	SELECT test_cs_free('pages', ARRAY(SELECT pack_h('\x50'::bytea || int4send(i))
		FROM generate_series(1, 10) i)) AS freed_found;
}

# new chunks
permutation o_put p_begin p_pack p_own_free o_free p_commit p_free
# put again in the transaction
permutation o_put p_begin p_pack p_pack_again o_free p_commit p_free
# referencing the store's: the leaves committed first, then the parents
# referencing them
permutation p_pack o_put p_begin p_parents o_free p_commit p_free_parents p_free
# all found, another's: held, and other chunks freed meanwhile
permutation p_pack o_put p_begin p_pack_again o_free o_free_found p_own_free p_commit p_free
