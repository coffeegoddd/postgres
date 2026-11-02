# A pack's region of pages freed, grown over the free pages after it as the
# pack reserves its chunks' bytes, which it writes once it is done: the
# region is the backend's from its first page still, so that VACUUM, its
# pass meanwhile, takes none of the pages with bytes reserved and not yet
# written for a region let go of (it would seal them, and the pack's write
# then find them full).  The pack stops, by an injection point of its
# session alone, as its region grows the second time.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE OR REPLACE FUNCTION pe_d(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT convert_to(rpad('pe' || i, 2000, '.'), 'UTF8')$$;
	CREATE OR REPLACE FUNCTION pe_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	SELECT count(*) FROM generate_series(1, 40) i, LATERAL test_cs_put('pages', pe_d(i));
	SELECT stored FROM test_cs_put('pages', pe_d(1000));
	SELECT test_cs_ref_root('pages', pe_h(pe_d(1000)));
}

# (freed in a transaction of its own: one's own puts are held by it)
setup
{
	SELECT test_cs_free('pages', ARRAY(SELECT pe_h(pe_d(i)) FROM generate_series(1, 40) i));
}

teardown
{
	SELECT test_cs_unref_root('pages', pe_h(pe_d(1000)));
	SELECT test_cs_free_unheld('pages');
}

session p
setup		{
	SET chunk_free_in_background = off;
	SELECT injection_points_set_local();
}
step p_attach	{ SELECT injection_points_attach('chunk-log-extended', 'wait'); }
step p_pack		{
	SELECT test_cs_put_many('pages', ARRAY(SELECT pe_h(pe_d(i)) FROM generate_series(101, 112) i),
		ARRAY(SELECT pe_d(i) FROM generate_series(101, 112) i)) AS stored;
}
step p_detach	{ SELECT injection_points_detach('chunk-log-extended'); }

session v
setup		{ SET chunk_free_in_background = off; }
step v_vacuum	{ VACUUM pg_history.chunk_index; }
step v_check	{
	SELECT ok, count(*) FILTER (WHERE test_cs_get('pages', pe_h(pe_d(i))) = pe_d(i)) AS read_back
	FROM test_cs_check('pages'), generate_series(101, 112) i GROUP BY ok;
}

session o
step o_wake		{ SELECT injection_points_wakeup('chunk-log-extended'); }

# the pages freed made free (VACUUM), the pack stopped as its region grows
# the first time and then the second, VACUUM's pass then, and the pack on
permutation v_vacuum p_attach p_pack o_wake v_vacuum o_wake p_detach v_check
