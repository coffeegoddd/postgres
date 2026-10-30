# Two sessions write the same shared pages at once, one through a
# block-mapped table and one through its copy, a table whose storage, and
# its index's, shares the table's.  Each side's first change to a shared
# page copies it; the other side must go on reading the page as it was
# shared.  Updates (HOT and not), deletes, rollbacks, on-access pruning
# and VACUUM, interleaved, each side seeing only its own rows, through a
# sequential scan and its index alike, and amcheck finding both sound.
# Steps run one at a time: this is what each side sees of the other's
# changes as they interleave; t/043 runs them at once.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE EXTENSION IF NOT EXISTS amcheck;
	SET debug_block_mapped_heap = on;
	CREATE TABLE t (k int PRIMARY KEY, side text, n int, pad text)
		WITH (fillfactor = 100, autovacuum_enabled = off);
	INSERT INTO t SELECT g, 'base', 0, repeat('p', 150) FROM generate_series(1, 400) g;
	UPDATE t SET n = 1 WHERE k % 40 = 0;
}

setup
{
	VACUUM (FREEZE) t;
}

setup
{
	SET debug_block_mapped_heap = on;
	CREATE TABLE c (LIKE t INCLUDING ALL) WITH (fillfactor = 100, autovacuum_enabled = off);
	UPDATE pg_class SET relfilenode = test_bmap_share(pg_relation_filenode('t')),
		relfrozenxid = (SELECT relfrozenxid FROM pg_class WHERE relname = 't'),
		relminmxid = (SELECT relminmxid FROM pg_class WHERE relname = 't')
		WHERE relname = 'c';
	UPDATE pg_class SET relfilenode = test_bmap_share(pg_relation_filenode('t_pkey'))
		WHERE relname = 'c_pkey';
}

teardown
{
	DROP TABLE t, c;
}

session s1
setup		{ SET enable_seqscan = on; }
step s1b	{ BEGIN; }
step s1u	{ UPDATE t SET side = 't', n = n + 1 WHERE k BETWEEN 1 AND 30; }
step s1u2	{ UPDATE t SET side = 't', n = n + 10 WHERE k BETWEEN 10 AND 12; }
step s1d	{ DELETE FROM t WHERE k BETWEEN 41 AND 80; }
step s1hot	{ UPDATE t SET n = n + 1 WHERE k = 200; }
step s1c	{ COMMIT; }
step s1r	{ ROLLBACK; }
step s1v	{ VACUUM t; }
step s1prune { SELECT count(*) FROM t WHERE k BETWEEN 190 AND 210; }
step s1chk
{
	SELECT 't' AS rel, side, count(*), sum(n), sum(k) FROM t GROUP BY side ORDER BY side;
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SELECT 't by index' AS rel, count(*), sum(n), sum(k) FROM t WHERE k BETWEEN 1 AND 400;
	RESET enable_seqscan; RESET enable_bitmapscan;
	SELECT count(*) AS t_heap_problems FROM verify_heapam('t');
	SELECT bt_index_check('t_pkey', true);
}

session s2
step s2shared
{
	SELECT count(*) FILTER (WHERE refs <> 0) > 0 AS heap_shared
		FROM test_bmap_map(pg_relation_filenode('c'));
	SELECT count(*) FILTER (WHERE refs <> 0) > 0 AS index_shared
		FROM test_bmap_map(pg_relation_filenode('c_pkey'));
}
step s2b	{ BEGIN; }
step s2u	{ UPDATE c SET side = 'c', n = n + 2 WHERE k BETWEEN 20 AND 50; }
step s2u2	{ UPDATE c SET side = 'c', n = n + 20 WHERE k BETWEEN 11 AND 13; }
step s2d	{ DELETE FROM c WHERE k BETWEEN 1 AND 15; }
step s2hot	{ UPDATE c SET n = n + 2 WHERE k = 200; }
step s2i	{ INSERT INTO c SELECT g, 'c', 5, repeat('q', 150) FROM generate_series(401, 440) g; }
step s2c	{ COMMIT; }
step s2r	{ ROLLBACK; }
step s2v	{ VACUUM c; }
step s2prune { SELECT count(*) FROM c WHERE k BETWEEN 190 AND 210; }
step s2chk
{
	SELECT 'c' AS rel, side, count(*), sum(n), sum(k) FROM c GROUP BY side ORDER BY side;
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SELECT 'c by index' AS rel, count(*), sum(n), sum(k) FROM c WHERE k BETWEEN 1 AND 440;
	RESET enable_seqscan; RESET enable_bitmapscan;
	SELECT count(*) AS c_heap_problems FROM verify_heapam('c');
	SELECT bt_index_check('c_pkey', true);
}

# Both sides change the same rows on the same pages, each in an open
# transaction, committing in either order.
permutation s2shared s1b s1u s2b s2u s1c s2c s1chk s2chk
permutation s1b s1u s2b s2u s2c s1c s1chk s2chk
permutation s2b s2u s1b s1u s1u2 s2u2 s1c s2c s1chk s2chk

# One side rolls back while the other commits: neither sees the other's
# changes, and the rolled-back side reads the rows as shared.
permutation s1b s1u s2b s2u s1r s2c s1chk s2chk
permutation s1b s1u s2b s2u s2r s1c s1chk s2chk

# Deletes on each side, then VACUUM of one side while the other side's
# transaction on the same pages is open, and of the other after.
permutation s1b s1d s2b s2d s2i s1c s1v s2c s2v s1chk s2chk
permutation s2b s2d s1b s1u s2c s2v s1c s1v s1chk s2chk

# HOT updates of the same row on each side, each side's reads pruning its
# own pages on access, then VACUUM of each.
permutation s1hot s1hot s2hot s1prune s2prune s2hot s1hot s2prune s1prune s1v s2v s1chk s2chk
permutation s1b s1hot s2hot s2prune s2v s1c s1prune s1v s1chk s2chk
