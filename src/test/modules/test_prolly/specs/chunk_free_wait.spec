# Freeing and putting at once: a put of a chunk being freed waits for the
# freeing to end, and then puts it anew (committed) or finds it (rolled
# back); a chunk a running transaction put, or found, is pinned, and not
# freed meanwhile.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE TABLE fl () USING prolly_chunks;
	CREATE TABLE fi () USING prolly_chunk_index;
	SELECT test_chunk_put('fl', 'fi', substr(sha256('\x01'), 1, 20), '\x01');
}

teardown
{
	DROP TABLE fl, fi;
}

session f
setup		{ SET chunk_free_in_background = off; }
step f_begin	{ BEGIN; }
step f_free		{ SELECT test_chunk_free('fl', 'fi', substr(sha256('\x01'), 1, 20)) AS freed; }
step f_commit	{ COMMIT; }
step f_rollback	{ ROLLBACK; }

session p
setup		{ SET chunk_free_in_background = off; }
step p_begin	{ BEGIN; }
step p_put		{ SELECT test_chunk_put('fl', 'fi', substr(sha256('\x01'), 1, 20), '\x01') AS stored; }
step p_ref		{ SELECT roots FROM test_chunk_ref_root('fi', substr(sha256('\x01'), 1, 20)); }
step p_commit	{ COMMIT; }
step p_vacuum	{ VACUUM fi; }
step p_unfrozen	{ SELECT test_chunk_index_unfrozen('fi') AS unfrozen; }

# a put of a chunk being freed waits, then puts it anew
permutation f_begin f_free p_put f_commit
# or finds it, the freeing rolled back
permutation f_begin f_free p_put f_rollback
# a chunk a running transaction found is pinned: not freed till it ends
permutation p_begin p_put f_free p_commit f_free
# and takes a root of it meanwhile, with no wait and no error
permutation p_begin p_put f_free p_ref p_commit f_free
# VACUUM freezes the entry's xmin while its freeing runs, which it leaves
permutation f_begin f_free p_vacuum p_unfrozen f_rollback p_vacuum p_unfrozen
