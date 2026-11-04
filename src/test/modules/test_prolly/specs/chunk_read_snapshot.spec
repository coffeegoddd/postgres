# A reader under a snapshot reads a chunk freed since it was taken, and what
# that chunk's freeing took with it; a snapshot taken after the freeing does
# not.  A chunk a reader's own transaction freed is gone to it, and a freeing
# rolled back frees nothing.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	SELECT test_chunk_format_refs(true);
	CREATE TABLE crs_l () USING prolly_chunks;
	CREATE TABLE crs_i () USING prolly_chunk_index;
	CREATE FUNCTION crs_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	-- (a chunk of the test format: 'R', how many it names, their hashes, a tag)
	CREATE FUNCTION crs_parent() RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT '\x5201'::bytea || crs_h('\x0a') || '\x70'::bytea$$;
	SELECT test_chunk_put('crs_l', 'crs_i', crs_h('\x0a'), '\x0a');
	SELECT test_chunk_put('crs_l', 'crs_i', crs_h(crs_parent()), crs_parent());
}

teardown
{
	DROP FUNCTION crs_parent();
	DROP FUNCTION crs_h(bytea);
	DROP TABLE crs_l, crs_i;
}

session r
setup		{ SET chunk_free_in_background = off; SELECT test_chunk_format_refs(true); }
step r_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) > 0 AS snapshot FROM pg_class; }
step r_parent	{ SELECT test_chunk_get('crs_l', 'crs_i', crs_h(crs_parent())) IS NOT NULL AS parent; }
step r_child	{ SELECT test_chunk_get('crs_l', 'crs_i', crs_h('\x0a')) AS child; }
step r_has		{ SELECT test_chunk_has('crs_l', 'crs_i', crs_h('\x0a')) AS has; }
step r_free		{ SELECT test_chunk_free('crs_l', 'crs_i', crs_h(crs_parent())) AS freed; }
step r_commit	{ COMMIT; }

session f
setup		{ SET chunk_free_in_background = off; SELECT test_chunk_format_refs(true); }
step f_begin	{ BEGIN; }
step f_free		{ SELECT test_chunk_free('crs_l', 'crs_i', crs_h(crs_parent())) AS freed; }
step f_put		{ SELECT test_chunk_put('crs_l', 'crs_i', crs_h('\x0a'), '\x0a') AS stored; }
step f_commit	{ COMMIT; }
step f_rollback	{ ROLLBACK; }

# freed since the snapshot: the chunk, and the child its freeing took; the store
# no longer says it has them, and a later snapshot does not read them
permutation r_begin f_free r_parent r_child r_has r_commit r_parent r_child
# the freeing not yet committed: read, and not waited for
permutation r_begin f_begin f_free r_parent r_child f_commit r_commit r_parent
# rolled back: frees nothing
permutation f_begin f_free f_rollback r_parent r_child
# freed by the reader itself: gone to it
permutation r_begin r_free r_parent r_child r_commit
# freed and put anew since the snapshot: the same bytes
permutation r_begin f_free f_put r_child r_commit r_child
