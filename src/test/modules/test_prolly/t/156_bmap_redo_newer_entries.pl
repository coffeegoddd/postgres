# Copyright (c) 2026, PostgreSQL Global Development Group

# Recovery's view of a map entry never goes backwards (#647).  A map page
# on disk can be newer than the record being replayed: an entry it has
# changed since (a block let go of on extension, given another block)
# makes redo skip the logical block's records from before the change.  A
# full-page image of the same map page, logged before the change (an
# extension of another block on it), must not bring the old entry back:
# the block's later records would then be replayed onto a page that
# missed the ones skipped.  wal_consistency_checking compares the map page
# once replay has caught up with the entries kept.  A torn newer page still
# takes the older records' changes of the entries it doesn't have, and an
# older record leaves alone an entry the newer page changed later.

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

# A newer map page can be torn: its header (and the entries in the same
# sector) newer than the record, the rest older.  The image restored over
# it keeps the newer entries and takes the newer page's LSN, so the
# records from before that LSN must still change the older entries.
# Checksums are off, as a torn page passes verification only then.
my $torn = PostgreSQL::Test::Cluster->new('redo_torn_newer');
$torn->init(no_data_checksums => 1);
$torn->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
});
$torn->start;

sub tsql { return $torn->safe_psql('postgres', $_[0]); }

tsql(q{CREATE EXTENSION test_prolly; CREATE EXTENSION pg_buffercache});
tsql(q{CREATE TABLE t (a int, b text) WITH (fillfactor = 10);
	INSERT INTO t SELECT g, repeat('x', 500) FROM generate_series(1, 60) g});
my $trel = tsql(q{SELECT pg_relation_filenode('t')});
my $tblocks = tsql(q{SELECT pg_relation_size('t') / 8192});
cmp_ok($tblocks, '>', 40, 'the table maps blocks past the map page\'s first sector');
# Its blocks shared with a copy, and its map page (the leaf of its map's
# tree, a block of the page store) copied to be its own again by a change.
my $tcopy = tsql(qq{SELECT test_bmap_share($trel)});
tsql(qq{SELECT test_bmap_write($trel, 0, 1)});
my $mapfile = $torn->data_dir . '/base/'
  . tsql(q{SELECT oid FROM pg_database WHERE datname = current_database()})
  . '/9421';
my $leaf = tsql(qq{SELECT block FROM test_bmap_tree($trel) WHERE level = 0 AND first = 0});
tsql('CHECKPOINT');

# The map page as the checkpoint wrote it.
sub map_page
{
	open(my $fh, '<:raw', $mapfile) or die "open $mapfile: $!";
	seek($fh, $leaf * 8192, 0) or die;
	read($fh, my $page, 8192) == 8192 or die "short read of $mapfile";
	close($fh);
	return $page;
}
my $before = map_page();

# The first change after the checkpoint logs the map page's image; then
# the table, cut to 5 blocks below the copy and grown back, gives every
# block from 5 on another block, in its first sector and past it.
tsql(qq{SELECT test_bmap_truncate($trel, 5)});
tsql(qq{SELECT test_bmap_extend($trel, $tblocks - 5)});
my $map = qq{SELECT string_agg(blkno || ':' || phys, ',' ORDER BY blkno)
	FROM test_bmap_map($trel)};
my $want = tsql($map);
is( tsql(
		qq{SELECT bool_and((pg_buffercache_evict(bufferid)).buffer_evicted)
		FROM pg_buffercache WHERE (relfilenode = $trel AND relforknumber = 4)
		   OR (relfilenode = 9421 AND relforknumber = 0 AND relblocknumber = $leaf)}),
	't',
	'the map is written out');
$torn->stop('immediate');

# Tear the map page: its first sector as written, the rest as before.
my $after = map_page();
isnt(substr($after, 512), substr($before, 512), 'the entries past the first sector changed');
open(my $fh, '+<:raw', $mapfile) or die "open $mapfile: $!";
seek($fh, $leaf * 8192 + 512, 0) or die;
print $fh substr($before, 512) or die;
close($fh) or die;

ok($torn->start(fail_ok => 1), 'recovery over the torn map page completes');
is(tsql($map), $want, 'and ends with the map as it was');

# A record from before the newer page's LSN leaves alone an entry the
# newer page changed later.  A claim (the entry's block kept, born again
# when its copy is gone) logged between the image and the later change
# would otherwise take the entry back to its old block, and the block's
# next record would be replayed onto a page that missed the ones skipped.
# Here the old block holds what it held at the checkpoint, as a base
# backup that copied the page store before the copy's write can have.
tsql(q{CREATE TABLE u (a int, b text) WITH (fillfactor = 50);
	INSERT INTO u SELECT g, repeat('x', 500) FROM generate_series(1, 150) g});
my $urel = tsql(q{SELECT pg_relation_filenode('u')});
my $uk = tsql(q{SELECT a FROM u WHERE ctid = '(10,1)'});
my $store = $torn->data_dir . '/base/'
  . tsql(q{SELECT oid FROM pg_database WHERE datname = current_database()})
  . '/9421';
tsql('CHECKPOINT');
my $b0 = tsql(qq{SELECT phys FROM test_bmap_map($urel) WHERE blkno = 10});
sub store_page
{
	my ($blk, $page) = @_;
	open(my $fh, defined $page ? '+<:raw' : '<:raw', $store) or die "open $store: $!";
	seek($fh, $blk * 8192, 0) or die;
	if (defined $page)
	{
		print $fh $page or die;
	}
	else
	{
		read($fh, $page, 8192) == 8192 or die "short read of $store";
	}
	close($fh) or die;
	return $page;
}
my $b0_page = store_page($b0);

# The map page's image; block 10's row updated twice; a copy made and
# dropped, so the next update claims block 10's block; and block 10 given
# another block, a later copy holding the old one.  A snapshot held keeps
# the last update from pruning the page, and a scan sets its hints before
# the copy: either would find the page no longer shared, and leave its
# entry unclaimed.
my $snap = $torn->background_psql('postgres');
$snap->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$snap->query_safe('SELECT count(*) FROM u');
tsql(qq{SELECT test_bmap_extend($urel, 1)});
tsql(qq{UPDATE u SET b = repeat('1', 500) WHERE a = $uk});
tsql(qq{UPDATE u SET b = repeat('2', 500) WHERE a = $uk});
tsql(q{SELECT count(*) FROM u});
tsql(qq{SELECT test_bmap_unlink(test_bmap_share($urel))});
tsql(qq{UPDATE u SET b = repeat('3', 500) WHERE a = $uk});
is(tsql(qq{SELECT phys FROM test_bmap_map($urel) WHERE blkno = 10}), $b0,
	'the update claims block 10\'s block');
tsql(qq{SELECT test_bmap_share($urel)});
tsql(qq{SELECT test_bmap_truncate($urel, 10)});
tsql(qq{SELECT test_bmap_extend($urel, 1)});
my $b2 = tsql(qq{SELECT phys FROM test_bmap_map($urel) WHERE blkno = 10});
isnt($b2, $b0, 'block 10 is given another block');
is( tsql(
		qq{SELECT bool_and((pg_buffercache_evict(bufferid)).buffer_evicted)
		FROM pg_buffercache WHERE relfilenode = $urel AND relforknumber = 4}),
	't',
	'the map is written out');
$snap->quit;
$torn->stop('immediate');
store_page($b0, $b0_page);

$log_offset = -s $torn->logfile;
ok($torn->start(fail_ok => 1), 'recovery past the claim completes');
ok(!$torn->log_contains('PANIC', $log_offset), 'without a PANIC');
is(tsql(qq{SELECT phys FROM test_bmap_map($urel) WHERE blkno = 10}), $b2,
	'block 10 keeps the block it was given');
$torn->stop;

done_testing();
