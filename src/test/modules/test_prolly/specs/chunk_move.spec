# Compaction moves a chunk's entry, freeing the old in its transaction and
# putting a new one: a chunk another transaction holds is passed by; a
# change of its counts, a put of it, or a read, as it is moved waits for
# the move (as for a freeing) and then is the new entry's; and a move
# rolled back leaves the old entry the chunk's.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE TABLE mcl () USING prolly_chunks;
	CREATE TABLE mci () USING prolly_chunk_index;
	CREATE FUNCTION mc_d(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT convert_to(rpad('mc' || i, 2000, '.'), 'UTF8')$$;
	CREATE FUNCTION mc_h(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(mc_d(i)), 1, 20)';
	SELECT count(*) FROM generate_series(1, 12) i, LATERAL test_chunk_put('mcl', 'mci', mc_h(i), mc_d(i));
	SELECT count(*) FROM generate_series(2, 12, 2) i, LATERAL test_chunk_ref_root('mci', mc_h(i));
}

# (freed in a transaction of their own, and their entries taken out: the
# log's first two pages half dead)
setup
{
	SELECT sum(test_chunk_free('mcl', 'mci', mc_h(i))) FROM generate_series(1, 12, 2) i;
}
setup		{ VACUUM mci; }

teardown
{
	DROP TABLE mcl, mci;
	DROP FUNCTION mc_h(int), mc_d(int);
}

session c
setup		{ SET chunk_free_in_background = off; }
step c_begin	{ BEGIN; }
step c_compact	{ SELECT chunks FROM test_chunk_compact('mcl', 'mci', 0.4, 0); }
step c_commit	{ COMMIT; }
step c_rollback	{ ROLLBACK; }

session h
setup		{ SET chunk_free_in_background = off; }
step h_begin	{ BEGIN; }
step h_hold		{ SELECT test_chunk_hold('mci', mc_h(2)) AS held; }
step h_commit	{ COMMIT; }

session r
setup		{ SET chunk_free_in_background = off; }
step r_root		{ SELECT roots FROM test_chunk_ref_root('mci', mc_h(4)); }
step r_put		{ SELECT test_chunk_put('mcl', 'mci', mc_h(4), mc_d(4)) AS stored; }
step r_read		{ SELECT test_chunk_get('mcl', 'mci', mc_h(4)) = mc_d(4) AS read; }

session v
setup		{ SET chunk_free_in_background = off; }
step v_vacuum	{ VACUUM mci; }
step v_check	{
	SELECT ok, chunks,
		(SELECT count(*) FILTER (WHERE test_chunk_get('mcl', 'mci', mc_h(i)) = mc_d(i))
		 FROM generate_series(2, 12, 2) i) AS read_back,
		(SELECT roots FROM test_chunk_index_entry('mci', mc_h(4))) AS roots_4
	FROM test_chunk_check('mcl', 'mci');
}

# every kept chunk on the pages moved
permutation c_compact v_vacuum v_check
# but one another transaction holds
permutation h_begin h_hold c_compact h_commit v_vacuum v_check
# a change of its counts as it is moved waits, and is the new entry's
permutation c_begin c_compact r_root c_commit v_vacuum v_check
# a put of it as it is moved waits, and finds it
permutation c_begin c_compact r_put c_commit v_check
# a read as it is moved waits; a move rolled back leaves the old entries
# the chunks'
permutation c_begin c_compact r_read c_rollback v_vacuum v_check
# ... or committed, the new
permutation c_begin c_compact r_read c_commit v_vacuum v_check
