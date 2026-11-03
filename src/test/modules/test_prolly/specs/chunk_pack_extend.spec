# A pack's region of a segment freed, grown over the free segments after it
# as the pack reserves its chunks' bytes, which it writes once it is done: the
# region is the backend's from its first page still, so that VACUUM, its
# pass meanwhile, takes none of the pages with bytes reserved and not yet
# written for a region let go of (it would seal them, and the pack's write
# then find them full).  The pack stops, by an injection point of its
# session alone, as its region grows the second time.  A log's free space
# is its lanes' (prolly/chunk_lane.h): the segments freed are the pack's
# backend's own lane's, written and freed by it.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE OR REPLACE FUNCTION pe_d(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT convert_to(rpad('pe' || i, 2000, '.'), 'UTF8')$$;
	CREATE OR REPLACE FUNCTION pe_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
}

teardown
{
	SELECT test_cs_unref_root('pages', pe_h(pe_d(5000)));
	SELECT test_cs_free_unheld('pages');
}

session p
setup		{
	SET chunk_free_in_background = off;
	SELECT injection_points_set_local();
}
step p_load		{
	SELECT count(*) FROM generate_series(1, 400) i, LATERAL test_cs_put('pages', pe_d(i));
	SELECT stored FROM test_cs_put('pages', pe_d(5000));
	SELECT test_cs_ref_root('pages', pe_h(pe_d(5000)));
	-- (its region let go of: the pack's takes a segment freed)
	SELECT test_chunk_log_let_go('pg_history.chunks');
}
# (freed in a transaction of its own: one's own puts are held by it)
step p_free		{
	SELECT test_cs_free('pages', ARRAY(SELECT pe_h(pe_d(i)) FROM generate_series(1, 400) i));
}
step p_attach	{ SELECT injection_points_attach('chunk-log-extended', 'wait'); }
step p_pack		{
	SELECT test_cs_put_many('pages', ARRAY(SELECT pe_h(pe_d(i)) FROM generate_series(1001, 1300) i),
		ARRAY(SELECT pe_d(i) FROM generate_series(1001, 1300) i)) AS stored;
}
step p_detach	{ SELECT injection_points_detach('chunk-log-extended'); }

session v
setup		{ SET chunk_free_in_background = off; }
step v_vacuum	{ VACUUM pg_history.chunk_index; }
step v_check	{
	SELECT ok, count(*) FILTER (WHERE test_cs_get('pages', pe_h(pe_d(i))) = pe_d(i)) AS read_back
	FROM test_cs_check('pages'), generate_series(1001, 1300) i GROUP BY ok;
}

session o
step o_wake		{ SELECT injection_points_wakeup('chunk-log-extended'); }

# the pages freed made free (VACUUM), the pack stopped as its region grows
# the first time and then the second, VACUUM's pass then, and the pack on
permutation p_load p_free v_vacuum p_attach p_pack o_wake v_vacuum o_wake p_detach v_check
