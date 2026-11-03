# A segment freed, taken for a chunk another put first (the segment left
# made afresh, never written), let go of when its backend next needs more
# room than it has: its pages made all dead, free again at once.  A log's
# free space is its lanes' (prolly/chunk_lane.h), each used by the backend
# appending to it: b's lane is the one with segments freed, b's put takes
# one, and a's put, in a's own lane, takes none of them.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE TABLE rl () USING prolly_chunks;
	CREATE TABLE ri () USING prolly_chunk_index;
	CREATE OR REPLACE FUNCTION rs_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE OR REPLACE FUNCTION rs_big(tag text, len int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT convert_to(rpad(tag, len, '.'), 'UTF8')$$;
}

# (dropped with its lanes: none is left)
teardown
{
	DROP TABLE rl, ri;
	SELECT count(*) AS lanes_left FROM pg_class WHERE relname LIKE 'rl_lane_%';
}

session a
setup		{ SET chunk_free_in_background = off; }
step a_begin	{ BEGIN; }
step a_put		{ SELECT test_chunk_put('rl', 'ri', rs_h(rs_big('x', 20000)), rs_big('x', 20000)) AS put; }
step a_commit	{ COMMIT; }

session b
setup		{ SET chunk_free_in_background = off; }
step b_load		{
	SELECT count(*) FILTER (WHERE test_chunk_put('rl', 'ri', rs_h(rs_big('a' || g, 20000)),
		rs_big('a' || g, 20000))) FROM generate_series(1, 30) g;
	SELECT test_chunk_put('rl', 'ri', rs_h(rs_big('anchor', 20000)), rs_big('anchor', 20000));
	SELECT test_chunk_ref_root('ri', rs_h(rs_big('anchor', 20000)));
	-- (b's region let go of: its next put takes a segment freed)
	SELECT test_chunk_log_let_go('rl');
}
# (freed in a transaction of its own: one's own puts are held by it)
step b_unheld	{ SELECT test_chunk_free_unheld('rl', 'ri'); }
step b_vacuum	{ VACUUM ri; }
step b_free		{ SELECT free_pages FROM test_chunk_log_space('rl'); }
step b_put		{ SELECT test_chunk_put('rl', 'ri', rs_h(rs_big('x', 20000)), rs_big('x', 20000)) AS put; }
step b_put_more	{ SELECT test_chunk_put('rl', 'ri', rs_h(rs_big('z', 700000)), rs_big('z', 700000)) AS put; }

# b's put takes a segment freed in its lane (32 pages, of the 73 free),
# waits for a's put of the chunk, in a's own lane, and finds it a's: b's
# segment left afresh.  Its next, longer than the room free in its lane
# (pages new for it), lets the segment go: its 32 pages free again
permutation b_load b_unheld b_vacuum b_free a_begin a_put b_put a_commit b_free b_put_more b_free
