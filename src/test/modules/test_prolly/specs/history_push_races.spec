# Pushes racing what they look at.  A push takes no root, so it waits for
# no pointer's change and none waits for it, but a pointer to a chunk it
# is freeing (which waits for the push, and finds it pushed); a chunk a
# pointer made meanwhile holds stays; two pushes of one chunk compose; and
# a bundle's name is one bundle's (a push of a name another is pushing
# waits for it, and fails if it commits).

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE OR REPLACE FUNCTION hr_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	SELECT stored FROM test_cs_put('pages', '\x0a3101');
	SELECT stored FROM test_cs_put('pages', '\x0a3102');
	SELECT stored FROM test_cs_put('pages', '\x0a3103');
	SELECT test_cs_ref_root('pages', hr_h('\x0a3102'));
	SELECT test_cs_ref_root('pages', hr_h('\x0a3103'));
	SELECT test_cs_ref_root('pages', hr_h('\x0a3103'));
}

teardown
{
	SELECT count(pg_history_release(name)) FROM pg_history.bundles WHERE name IN ('hr', 'hr2');
	DELETE FROM pg_history.pointers WHERE name IN ('hr_ptr', 'hr_ptr2');
	SELECT count(*) FROM unnest(ARRAY['\x0a3101', '\x0a3102', '\x0a3103', '\x0a3103']::bytea[]) d,
		LATERAL (SELECT test_cs_unref_root('pages', hr_h(d)) WHERE test_cs_has('pages', hr_h(d))
			AND (test_cs_counts('pages', hr_h(d))).roots > 0) x;
	SELECT test_cs_free('pages', ARRAY[hr_h('\x0a3101'), hr_h('\x0a3102'), hr_h('\x0a3103')]);
}

session p
setup			{ SET chunk_free_in_background = off; }
step p_begin	{ BEGIN; }
step p_rr		{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1 AS snapshot; }
step p_push1	{ SELECT pg_history_push('hr', ARRAY[hr_h('\x0a3101')]) AS copied; }
step p_push2	{
	SELECT test_cs_unref_root('pages', hr_h('\x0a3102'));
	SELECT pg_history_push('hr', ARRAY[hr_h('\x0a3102')]) AS copied;
}
step p_push3	{
	SELECT test_cs_unref_root('pages', hr_h('\x0a3103'));
	SELECT pg_history_push('hr', ARRAY[hr_h('\x0a3103')]) AS copied;
}
step p_commit	{ COMMIT; }
step p_kept		{ SELECT test_cs_has('pages', hr_h('\x0a3101')) AS kept, test_cs_has('pages', hr_h('\x0a3103')) AS kept3; }

session q
setup			{ SET chunk_free_in_background = off; }
step q_begin	{ BEGIN; }
step q_pointer	{ INSERT INTO pg_history.pointers VALUES ('hr_ptr', hr_h('\x0a3101')); }
step q_pointer2	{ INSERT INTO pg_history.pointers VALUES ('hr_ptr2', hr_h('\x0a3101')); }
step q_pointer_pushed	{ INSERT INTO pg_history.pointers VALUES ('hr_ptr2', hr_h('\x0a3102')); }
step q_push		{ SELECT pg_history_push('hr', ARRAY[hr_h('\x0a3103')]) AS copied; }
step q_push3	{
	SELECT test_cs_unref_root('pages', hr_h('\x0a3103'));
	SELECT pg_history_push('hr2', ARRAY[hr_h('\x0a3103')]) AS copied;
}
step q_commit	{ COMMIT; }

# a pointer made since the pusher's snapshot: the chunk it holds stays
permutation p_rr q_pointer p_push1 p_commit p_kept
# a pointer to another chunk, as a push runs, and a push as a pointer is
# being changed: neither waits
permutation p_begin p_push2 q_pointer2 p_commit
permutation q_begin q_pointer2 p_push2 q_commit
# a pointer to a chunk being pushed and freed: waits, and finds it pushed
permutation p_begin p_push2 q_pointer_pushed p_commit
# two pushes of one chunk, each letting go of a root of it: compose, the
# chunk freed once neither holds it
permutation p_begin p_push3 q_begin q_push3 p_commit q_commit p_kept
# a push of the name another is pushing: waits, and fails once it commits
permutation p_begin p_push2 q_push p_commit
