# A pack whose put fails after one of its entries is on its page (an error
# in the bucket's split that entry starts, by an injection point of this
# session's): of its bytes, those reserved for the entries it put (rolled
# back), written or not, VACUUM counts dead, each once, and the room left
# goes to the next put.  A chunk put after it keeps the log's tail, which VACUUM
# would otherwise cut.  (Enough chunks that one splits a bucket whatever
# the index has seen: more than its buckets' first pages hold, a bucket
# split once one goes past its first page; buckets are never merged.)

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE OR REPLACE FUNCTION pack_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(test_engine_chunk(d)), 1, 20)';
}

session e
setup		{
	SET chunk_free_in_background = off;
	SELECT injection_points_set_local();
}
step e_start	{ VACUUM pg_history.chunk_index; }
step e_mark		{
	DROP TABLE IF EXISTS dead0, packn;
	CREATE TEMP TABLE dead0 AS SELECT dead FROM test_chunk_log_space('pg_history.chunks');
	-- (an entry is 56 bytes: fewer than 150 a page)
	CREATE TEMP TABLE packn AS SELECT buckets * 150 + 1 AS n
		FROM test_chunk_index_shape('pg_history.chunk_index');
}
step e_attach	{ SELECT injection_points_attach('chunk-index-split-published', 'error'); }
step e_pack		{
	SELECT test_cs_put_many('pages',
		ARRAY(SELECT pack_h('\x4c'::bytea || int4send(i)) FROM generate_series(1, (SELECT n FROM packn)) i ORDER BY i),
		ARRAY(SELECT test_engine_chunk(x) FROM unnest(ARRAY(SELECT '\x4c'::bytea || int4send(i) FROM generate_series(1, (SELECT n FROM packn)) i ORDER BY i)) WITH ORDINALITY u(x, o) ORDER BY o));
}
step e_detach	{ SELECT injection_points_detach('chunk-index-split-published'); }
step e_live		{ SELECT stored FROM test_cs_put('pages', test_engine_chunk('\x0a0401')); }
step e_vacuum	{ VACUUM pg_history.chunk_index; }
step e_dead		{
	-- (each chunk 7 bytes: the engine's page tag and version, 'L', four)
	SELECT more_dead > 0 AND more_dead < (SELECT n FROM packn) * 7 AND more_dead % 7 = 0
		AS reserved_ones_dead
	FROM (SELECT l.dead - d.dead AS more_dead
		  FROM test_chunk_log_space('pg_history.chunks') l, dead0 d) x;
}
step e_same		{
	DROP TABLE IF EXISTS dead1;
	CREATE TEMP TABLE dead1 AS SELECT dead FROM test_chunk_log_space('pg_history.chunks');
}
step e_again	{
	SELECT l.dead = d.dead AS counted_once FROM test_chunk_log_space('pg_history.chunks') l, dead1 d;
}
step e_none		{
	SELECT count(*) FILTER (WHERE test_cs_has('pages', pack_h('\x4c'::bytea || int4send(i)))) AS there
	FROM generate_series(1, (SELECT n FROM packn)) i;
}
step e_free		{ SELECT test_cs_free('pages', ARRAY[pack_h('\x0a0401')]) AS freed; }

permutation e_start e_mark e_attach e_pack e_detach e_live e_vacuum e_dead e_same e_vacuum e_again e_none e_free
