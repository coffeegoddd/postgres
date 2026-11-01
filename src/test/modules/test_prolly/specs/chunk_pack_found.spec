# A pack's chunks the store has are held until its transaction ends: those
# no other of the pack the store has references by changes adding nothing,
# the others by the references of one held.  Another's freeing of them
# meanwhile frees nothing.  A parent freed between the pack's finding it
# and holding it (the pack stopped there, by an injection point of its
# session alone) holds nothing: it is put again, and its leaf is held by
# the other parent, held.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE OR REPLACE FUNCTION pf_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	-- (a leaf, and two parents of it: the second of another leaf too)
	CREATE OR REPLACE FUNCTION pf_p1() RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT '\x5201'::bytea || pf_h('\x0a0901')$$;
	CREATE OR REPLACE FUNCTION pf_p2() RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT '\x5202'::bytea || pf_h('\x0a0901') || pf_h('\x0a0902')$$;
	SELECT test_chunk_format_refs(true);
	SELECT stored FROM test_cs_put('pages', '\x0a0901');
	SELECT stored FROM test_cs_put('pages', '\x0a0902');
	SELECT stored FROM test_cs_put('pages', pf_p1());
	SELECT stored FROM test_cs_put('pages', pf_p2());
}

teardown
{
	SELECT test_chunk_format_refs(true);
	SELECT test_cs_free('pages', ARRAY[pf_h(pf_p1()), pf_h(pf_p2()),
		pf_h('\x0a0901'), pf_h('\x0a0902')]);
	SELECT test_chunk_format_refs(false);
}

session p
setup		{
	SET chunk_free_in_background = off;
	SELECT test_chunk_format_refs(true);
	SELECT injection_points_set_local();
}
step p_attach	{ SELECT injection_points_attach('chunk-pack-found', 'wait'); }
step p_begin	{ BEGIN; }
step p_pack		{
	SELECT test_cs_put_many('pages', ARRAY[pf_h(pf_p1()), pf_h(pf_p2()), pf_h('\x0a0901')],
		ARRAY[pf_p1(), pf_p2(), '\x0a0901'::bytea]) AS stored;
}
step p_commit	{ COMMIT; SELECT injection_points_detach('chunk-pack-found'); }

session q
setup		{ SET chunk_free_in_background = off; SELECT test_chunk_format_refs(true); }
step q_begin	{ BEGIN; }
step q_pack		{
	SELECT test_cs_put_many('pages', ARRAY[pf_h(pf_p1()), pf_h(pf_p2()), pf_h('\x0a0901')],
		ARRAY[pf_p1(), pf_p2(), '\x0a0901'::bytea]) AS stored;
}
step q_commit	{ COMMIT; }

session o
setup		{ SET chunk_free_in_background = off; SELECT test_chunk_format_refs(true); }
step o_free_p1	{ SELECT test_cs_free('pages', ARRAY[pf_h(pf_p1())]) AS freed_p1; }
step o_wake		{ SELECT injection_points_wakeup('chunk-pack-found'); }
step o_free_all	{
	SELECT test_cs_free('pages', ARRAY[pf_h(pf_p1()), pf_h(pf_p2()), pf_h('\x0a0901')]) AS freed;
}
step o_has		{
	SELECT test_cs_has('pages', pf_h(pf_p1())) AS p1, test_cs_has('pages', pf_h('\x0a0901')) AS leaf;
}

# another's freeing meanwhile frees nothing of the pack's
permutation q_begin q_pack o_free_all q_commit o_has
# a parent freed between the pack's finding and holding it: put again,
# the leaf held by the other parent
permutation p_attach p_begin p_pack o_free_p1 o_wake o_free_all(p_pack) p_commit o_has
