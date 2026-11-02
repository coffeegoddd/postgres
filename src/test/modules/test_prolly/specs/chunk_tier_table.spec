# A tier's pack's table has an entry for each chunk written in it, even
# when the chunks are written together: a pack of three (A, longest, first
# by hash) stopped as it finds what the tier has, another session putting
# A meanwhile, so that A's put fails and B and C, which fit A's room, are
# put one after another.  Each entry of every pack names one chunk, its
# bytes its hash's; and the place laid out for A, unused, is counted dead.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE OR REPLACE FUNCTION tt_a() RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT decode(repeat('0a', 99) || '0000', 'hex')$$;
	CREATE OR REPLACE FUNCTION tt_b() RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT decode(repeat('0b', 30), 'hex')$$;
	CREATE OR REPLACE FUNCTION tt_c() RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT decode(repeat('0c', 40), 'hex')$$;
	CREATE OR REPLACE FUNCTION tt_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	-- (the packs made before)
	CREATE TABLE tt_before AS
		SELECT coalesce(max(id), 0) AS id FROM test_tier_packs('pg_history.tier_chunks');
}

teardown
{
	SELECT test_cs_free('tier', ARRAY[tt_h(tt_a()), tt_h(tt_b()), tt_h(tt_c())]);
	DROP TABLE tt_before;
}

session p
setup		{ SET chunk_free_in_background = off; SELECT injection_points_set_local(); }
step p_attach	{ SELECT injection_points_attach('chunk-pack-found', 'wait'); }
step p_begin	{ BEGIN; }
step p_pack		{
	SELECT test_cs_put_many('tier', ARRAY[tt_h(tt_a()), tt_h(tt_b()), tt_h(tt_c())],
		ARRAY[tt_a(), tt_b(), tt_c()]) AS stored;
}
step p_packed	{ SELECT 'packed' AS p; }
step p_commit	{ COMMIT; SELECT injection_points_detach('chunk-pack-found'); }

session q
setup		{ SET chunk_free_in_background = off; }
step q_put_a	{ SELECT stored FROM test_cs_put('tier', tt_a()); }
step q_wake		{ SELECT injection_points_wakeup('chunk-pack-found'); }
step q_dead		{
	SELECT size, dead FROM test_tier_packs('pg_history.tier_chunks')
	WHERE id > (SELECT id FROM tt_before) AND size > 0 ORDER BY id;
}
step q_tables	{
	SELECT count(*) AS entries,
		count(*) FILTER (WHERE substr(sha256(test_cs_get('tier', t.hash)), 1, 20) = t.hash
			AND length(test_cs_get('tier', t.hash)) = t.len) AS each_one_chunk,
		count(DISTINCT t.hash) FILTER (WHERE t.hash IN (tt_h(tt_a()), tt_h(tt_b()), tt_h(tt_c()))) AS of_the_three
	FROM test_tier_packs('pg_history.tier_chunks') p,
		LATERAL test_tier_pack_table('pg_history.tier_chunks', p.id) t
	WHERE p.state <> 'removed' AND p.size > 0 AND p.id > (SELECT id FROM tt_before);
}

permutation p_attach p_begin p_pack q_put_a q_wake p_packed p_commit q_tables q_dead
