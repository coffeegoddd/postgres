# A chunk another transaction is putting: a put of it waits for that
# transaction to end, as an insert into a unique index does, and then finds
# it there (committed) or puts it (aborted).

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE TABLE cl () USING prolly_chunks;
	CREATE TABLE ci () USING prolly_chunk_index;
}

teardown
{
	DROP TABLE cl, ci;
}

session a
step a_begin	{ BEGIN; }
step a_put		{ SELECT test_chunk_put('cl', 'ci', substr(sha256('\x01'), 1, 20), '\x01'); }
step a_commit	{ COMMIT; }
step a_rollback	{ ROLLBACK; }

session b
step b_has		{ SELECT test_chunk_has('cl', 'ci', substr(sha256('\x01'), 1, 20)); }
step b_put		{ SELECT test_chunk_put('cl', 'ci', substr(sha256('\x01'), 1, 20), '\x01'); }

# another's chunk is not found until it commits; a put of it waits, then
# finds it
permutation a_begin a_put b_has b_put a_commit b_has
# or, if it aborts, puts it
permutation a_begin a_put b_put a_rollback b_has
