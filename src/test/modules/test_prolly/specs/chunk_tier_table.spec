# A tier's pack's table has an entry for each chunk written in it, even
# when the chunks are written together: a pack of three (A, B, C, laid out
# so) stopped as it finds what the tier has, another session putting one
# of them meanwhile, so that its put fails.  Each entry of every pack names
# one chunk, its bytes its hash's; and the place laid out for the one put
# elsewhere, unused, is counted dead: A's at the pack's start, B's amid it,
# a hole in each of two packs put in one transaction (A's, then E's of E
# and D).  C's, at the end, is no place: the pack ends before it.

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
	CREATE OR REPLACE FUNCTION tt_d() RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT decode(repeat('0d', 50), 'hex')$$;
	CREATE OR REPLACE FUNCTION tt_e() RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT decode(repeat('0e', 60), 'hex')$$;
	CREATE OR REPLACE FUNCTION tt_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(test_engine_chunk(d)), 1, 20)';
	-- (the packs made before)
	CREATE TABLE tt_before AS
		SELECT coalesce(max(id), 0) AS id FROM test_tier_packs('pg_history.tier_chunks');
}

teardown
{
	SELECT test_cs_free('tier', ARRAY[tt_h(tt_a()), tt_h(tt_b()), tt_h(tt_c()),
		tt_h(tt_d()), tt_h(tt_e())]);
	DROP TABLE tt_before;
}

session p
setup		{ SET chunk_free_in_background = off; SELECT injection_points_set_local(); }
step p_attach	{ SELECT injection_points_attach('chunk-pack-found', 'wait'); }
step p_begin	{ BEGIN; }
step p_pack		{
	SELECT test_cs_put_many('tier', ARRAY[tt_h(tt_a()), tt_h(tt_b()), tt_h(tt_c())],
		ARRAY(SELECT test_engine_chunk(x) FROM unnest(ARRAY[tt_a(), tt_b(), tt_c()]) WITH ORDINALITY u(x, o) ORDER BY o)) AS stored;
}
step p_pack2	{
	SELECT test_cs_put_many('tier', ARRAY[tt_h(tt_e()), tt_h(tt_d())],
		ARRAY(SELECT test_engine_chunk(x) FROM unnest(ARRAY[tt_e(), tt_d()]) WITH ORDINALITY u(x, o) ORDER BY o)) AS stored;
}
step p_packed	{ SELECT 'packed' AS p; }
step p_commit	{ COMMIT; SELECT injection_points_detach('chunk-pack-found'); }

session q
setup		{ SET chunk_free_in_background = off; }
step q_put_a	{ SELECT stored FROM test_cs_put('tier', test_engine_chunk(tt_a())); }
step q_put_b	{ SELECT stored FROM test_cs_put('tier', test_engine_chunk(tt_b())); }
step q_put_c	{ SELECT stored FROM test_cs_put('tier', test_engine_chunk(tt_c())); }
step q_put_e	{ SELECT stored FROM test_cs_put('tier', test_engine_chunk(tt_e())); }
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
permutation p_attach p_begin p_pack q_put_b q_wake p_packed p_commit q_tables q_dead
permutation p_attach p_begin p_pack q_put_c q_wake p_packed p_commit q_tables q_dead
permutation p_attach p_begin p_pack q_put_a q_wake p_packed p_pack2 q_put_e q_wake p_packed p_commit q_dead
