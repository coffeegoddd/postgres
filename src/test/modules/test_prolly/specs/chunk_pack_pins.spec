# A pack of many new chunks, its transaction open, pins them for its own
# backend alone (no other backend's freer frees chunks whose entries are
# not committed): its table of pins does not fill, which would count as
# pinning every chunk, and another's freeing goes on.  Its own freeing
# keeps them.

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
step p_commit	{ COMMIT; }
step p_free		{
	SELECT test_cs_free('pages', ARRAY(SELECT pack_h('\x50'::bytea || int4send(i))
		FROM generate_series(1, 3000) i)) AS freed;
}

session o
setup		{ SET chunk_free_in_background = off; }
step o_put		{ SELECT stored FROM test_cs_put('pages', '\x0a0501'); }
step o_free		{ SELECT test_cs_free('pages', ARRAY[pack_h('\x0a0501')]) AS freed; }

permutation o_put p_begin p_pack p_own_free o_free p_commit p_free
