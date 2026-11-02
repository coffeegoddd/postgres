# A run of pages freed, taken for a chunk another put first (the run left
# made afresh, never written), let go of when its backend next needs more
# room than it has: its pages made all dead, free again at once.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE TABLE rl () USING prolly_chunks;
	CREATE TABLE ri () USING prolly_chunk_index;
	CREATE OR REPLACE FUNCTION rs_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE OR REPLACE FUNCTION rs_big(tag text, len int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT convert_to(rpad(tag, len, '.'), 'UTF8')$$;
	SELECT count(*) FILTER (WHERE test_chunk_put('rl', 'ri', rs_h(rs_big('a' || g, 20000)),
		rs_big('a' || g, 20000))) FROM generate_series(1, 30) g;
	SELECT test_chunk_put('rl', 'ri', rs_h(rs_big('anchor', 20000)), rs_big('anchor', 20000));
	SELECT test_chunk_ref_root('ri', rs_h(rs_big('anchor', 20000)));
}

# (freed in a transaction of its own: one's own puts are held by it)
setup
{
	SELECT test_chunk_free_unheld('rl', 'ri');
}

teardown
{
	DROP TABLE rl, ri;
}

session a
setup		{ SET chunk_free_in_background = off; }
step a_begin	{ BEGIN; }
step a_put		{ SELECT test_chunk_put('rl', 'ri', rs_h(rs_big('x', 20000)), rs_big('x', 20000)) AS put; }
step a_commit	{ COMMIT; }

session b
setup		{ SET chunk_free_in_background = off; }
step b_vacuum	{ VACUUM ri; }
step b_free		{ SELECT free_pages FROM test_chunk_log_space('rl'); }
step b_put		{ SELECT test_chunk_put('rl', 'ri', rs_h(rs_big('x', 20000)), rs_big('x', 20000)) AS put; }
step b_put_more	{ SELECT test_chunk_put('rl', 'ri', rs_h(rs_big('z', 330000)), rs_big('z', 330000)) AS put; }

# b's put takes a run of three pages freed, waits for a's, and finds the
# chunk a's: the run left afresh.  Its next, longer than a region (pages new
# for it), lets the run go: its three pages free again
permutation b_vacuum b_free a_begin a_put b_put a_commit b_free b_put_more b_free
