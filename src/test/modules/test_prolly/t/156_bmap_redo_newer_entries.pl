# Copyright (c) 2026, PostgreSQL Global Development Group

# Recovery's view of a map entry never goes backwards (#647).  A map page
# on disk can be newer than the record being replayed: an entry it has
# changed since (a block let go of on extension, given another block)
# makes redo skip the logical block's records from before the change.  A
# full-page image of the same map page, logged before the change (an
# extension of another block on it), must not bring the old entry back:
# the block's later records would then be replayed onto a page that
# missed the ones skipped.  wal_consistency_checking compares the map page
# once replay has caught up with the entries kept.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('redo_newer_entries');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
wal_consistency_checking = 'all'
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly; CREATE EXTENSION pg_buffercache});
sql(q{CREATE TABLE t (a int, b text) WITH (fillfactor = 50);
	INSERT INTO t SELECT g, repeat('x', 500) FROM generate_series(1, 150) g});
my $rel = sql(q{SELECT pg_relation_filenode('t')});
my $nblocks = sql(q{SELECT pg_relation_size('t') / 8192});
cmp_ok($nblocks, '>', 11, 'the table has more than 11 blocks');
my $k = sql(q{SELECT a FROM t WHERE ctid = '(10,1)'});
sql('CHECKPOINT');

# Block 10's row updated in place three times (its image with the first),
# and an extension of the table between the second and the third: the
# map page's image, with block 10's entry as it is now.
sql(qq{UPDATE t SET b = repeat('1', 500) WHERE a = $k});
sql(qq{UPDATE t SET b = repeat('2', 500) WHERE a = $k});
sql(q{INSERT INTO t SELECT g, repeat('y', 500) FROM generate_series(1001, 1050) g});
cmp_ok(sql(q{SELECT pg_relation_size('t') / 8192}), '>', $nblocks,
	'an extension maps another block on the same map page');
sql(qq{UPDATE t SET b = repeat('3', 500) WHERE a = $k});
is(sql(qq{SELECT (ctid::text::point)[0] FROM t WHERE a = $k}), '10',
	'the row stays on block 10');

# A copy holds block 10's block, so the table, cut to 10 blocks below the
# catalogs and grown by one, lets go of it: block 10 is given another
# block.  The map, its header too, reaches disk; the table's pages don't
# need to.
my $copy = sql(qq{SELECT test_bmap_share($rel)});
my $copy_rows = sql(qq{SELECT test_bmap_rows_digest($copy)});
my $old = sql(qq{SELECT phys FROM test_bmap_map($rel) WHERE blkno = 10});
sql(qq{SELECT test_bmap_truncate($rel, 10)});
sql(qq{SELECT test_bmap_extend($rel, 1)});
isnt(sql(qq{SELECT phys FROM test_bmap_map($rel) WHERE blkno = 10}), $old,
	'block 10 is given another block');
is( sql(
		qq{SELECT bool_and((pg_buffercache_evict(bufferid)).buffer_evicted)
		FROM pg_buffercache WHERE relfilenode = $rel AND relforknumber = 4}),
	't',
	'the map is written out');

# Replay skips block 10's second update (the map on disk gave it another
# block since), restores the map page's image from the extension, and
# replays the third update.
$node->stop('immediate');
my $log_offset = -s $node->logfile;
ok($node->start(fail_ok => 1), 'recovery completes');
ok(!$node->log_contains('PANIC', $log_offset), 'without a PANIC');
is(sql(q{SELECT pg_relation_size('t') / 8192}), '11', 'the table has its length');
is(sql(qq{SELECT count(*) FROM test_bmap_map($rel) WHERE phys = $old}), '0',
	'block 10 keeps the block it was given');
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $copy_rows,
	'the copy keeps the rows it was made with');
sql(q{INSERT INTO t SELECT g, repeat('z', 500) FROM generate_series(2001, 2100) g});
is(sql(q{SELECT count(*) FROM t WHERE a > 2000}), '100', 'and the table extends again');

$node->stop;
done_testing();
