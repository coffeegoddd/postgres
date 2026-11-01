# Letting go of a chunk's root waits for another transaction changing its
# counts, as a row's update waits for another's: two letting go of its last
# root would otherwise both see it, and take it below zero.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE TABLE rl () USING prolly_chunks;
	CREATE TABLE ri () USING prolly_chunk_index;
	SELECT test_chunk_put('rl', 'ri', substr(sha256('\x01'), 1, 20), '\x01');
	SELECT test_chunk_ref_root('ri', substr(sha256('\x01'), 1, 20));
}

teardown
{
	DROP TABLE rl, ri;
}

session a
step a_begin	{ BEGIN; }
step a_unref	{ SELECT * FROM test_chunk_unref_root('ri', substr(sha256('\x01'), 1, 20)); }
step a_ref		{ SELECT * FROM test_chunk_ref_root('ri', substr(sha256('\x01'), 1, 20)); }
step a_commit	{ COMMIT; }
step a_rollback	{ ROLLBACK; }

session b
step b_unref	{ SELECT * FROM test_chunk_unref_root('ri', substr(sha256('\x01'), 1, 20)); }
step b_ref		{ SELECT * FROM test_chunk_ref_root('ri', substr(sha256('\x01'), 1, 20)); }
step b_vacuum	{ VACUUM ri; }
step b_unfrozen	{ SELECT test_chunk_index_unfrozen('ri') AS unfrozen; }

# the second to let go of the last root waits, then is refused
permutation a_begin a_unref b_unref a_commit
# or, the first rolled back, lets go of it
permutation a_begin a_unref b_unref a_rollback
# taking a root doesn't wait
permutation a_begin a_ref b_ref a_commit
# letting go doesn't wait for another's taking one: that can only add
permutation a_begin a_ref b_unref a_commit
# VACUUM leaves another's open change, and adds it in once committed
permutation a_begin a_ref b_vacuum b_unfrozen a_commit b_vacuum b_unfrozen
