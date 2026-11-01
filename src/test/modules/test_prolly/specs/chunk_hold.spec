# A chunk a put finds stored is held by a change of its counts adding
# nothing: a freeing of it as it is found is waited for (the chunk, freed,
# is put again; the freeing rolled back, it is found); and a hold of a
# subtransaction rolled back holds nothing more.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	SELECT stored FROM test_cs_put('pages', '\x0a0801');
}

teardown
{
	SELECT test_cs_free('pages', ARRAY[substr(sha256('\x0a0801'), 1, 20)]);
}

session a
setup		{ SET chunk_free_in_background = off; }
step a_begin	{ BEGIN; }
step a_free		{ SELECT test_cs_free('pages', ARRAY[substr(sha256('\x0a0801'), 1, 20)]) AS freed; }
step a_save		{ SAVEPOINT s; }
step a_hold		{ SELECT test_chunk_hold('pg_history.chunk_index', substr(sha256('\x0a0801'), 1, 20)) AS held; }
step a_rollback_to	{ ROLLBACK TO s; }
step a_commit	{ COMMIT; }
step a_rollback	{ ROLLBACK; }

session b
setup		{ SET chunk_free_in_background = off; }
step b_put		{ SELECT stored FROM test_cs_put('pages', '\x0a0801'); }
step b_free		{ SELECT test_cs_free('pages', ARRAY[substr(sha256('\x0a0801'), 1, 20)]) AS freed; }

# a put finding the chunk as it is being freed waits: freed, it is put
permutation a_begin a_free b_put a_commit
# ... the freeing rolled back, it is found
permutation a_begin a_free b_put a_rollback
# a hold of a subtransaction rolled back holds nothing more
permutation a_begin a_save a_hold a_rollback_to b_free a_commit b_put
