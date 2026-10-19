# Copyright (c) 2026, PostgreSQL Global Development Group

# Crash recovery's prefetch of a block whose map page is still zeroes on
# disk.  An extension that adds a map page logs the page and the map's
# header in one record, and either can reach disk first; with the header
# written and the new page not, the page is zeroes on disk, below the map's
# length, until the record is replayed.  The WAL prefetcher looks blocks up
# ahead of replay, and a lookup of a block on that page finds no entry: the
# prefetch is skipped, not a failure of recovery.  Outside recovery, a map
# page of zeroes under the length is a lost write, and a prefetch through
# it fails.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Nothing writes out a dirty buffer but the test's evictions: map page 2
# stays zeroes on disk until the crash.
my $node = PostgreSQL::Test::Cluster->new('prefetch');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
bgwriter_lru_maxpages = 0
checkpoint_timeout = 1d
max_wal_size = 10GB
shared_buffers = 128MB
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# Without read-ahead advice there is no prefetcher to test: the server
# refuses recovery_prefetch = on (taking effect at the crash's restart)
# where it has none, as it does without posix_fadvise() (macOS, Windows).
my ($ret, $stdout, $stderr) = $node->psql('postgres',
	'ALTER SYSTEM SET recovery_prefetch = on');
if ($ret != 0)
{
	$node->stop;
	plan skip_all => 'recovery_prefetch is not supported on this platform';
}

# One row a block (half a block, stored inline), and as many blocks as a
# map page has entries: (block size - page header - special space) / entry.
my $bs = sql(q{SELECT current_setting('block_size')});
my $fanout = ($bs - 24 - 8) / 16;
my $half = $bs / 2;
sql(qq{CREATE EXTENSION pg_buffercache;
	CREATE TABLE c (a int, b text);
	ALTER TABLE c ALTER b SET STORAGE PLAIN;
	INSERT INTO c SELECT g, repeat('x', $half) FROM generate_series(1, $fanout) g});
is(sql(q{SELECT pg_relation_size('c') / current_setting('block_size')::int}),
	$fanout, 'the table has the whole of map page 1');

# The next block adds map page 2, after the checkpoint recovery starts at.
# The prefetcher looks up ahead of replay the blocks of records that
# neither initialize nor carry a full image of their page: here the locks
# of the rows on map page 2's blocks, after their pages' first changes.
# (By ctid: a scan of the table would log its hint bits' page images,
# taking the locks past where the prefetcher reads ahead.)
sql('CHECKPOINT');
sql(qq{INSERT INTO c VALUES (0, repeat('a', $half))});
sql(qq{INSERT INTO c VALUES (0, repeat('b', $half))});
my $next = $fanout + 1;
is( sql(qq{SELECT count(*) FROM (SELECT FROM c
		WHERE ctid = ANY ('{"($fanout,1)","($next,1)"}'::tid[]) AND a = 0
		FOR UPDATE) l}),
	'2',
	'the rows on map page 2\'s blocks are locked');

# The map's header reaches disk; map page 2 doesn't.
ok( $node->poll_query_until(
		'postgres', q{SELECT bool_and((pg_buffercache_evict(bufferid)).buffer_evicted) FROM pg_buffercache
		WHERE relfilenode = pg_relation_filenode('c')
		  AND relforknumber = 4 AND relblocknumber = 0}),
	'the map\'s header is written out');
my $mappath = $node->data_dir . '/' . sql(q{SELECT pg_relation_filepath('c')}) . '_bmap';

$node->stop('immediate');
open(my $fh, '<:raw', $mappath) or die "could not open $mappath: $!";
seek($fh, 2 * $bs, 0);
my $page;
is(read($fh, $page, $bs), $bs, 'map page 2 is in the file');
close($fh);
is($page, "\0" x $bs, 'and is zeroes on disk at the crash');

my $log_offset = -s $node->logfile;
$node->start(fail_ok => 1);
ok(!$node->log_contains('is not an initialized block map page', $log_offset),
	'recovery prefetches past the zeroed map page');
is(sql(q{SELECT count(*) FROM c}), $fanout + 2, 'and recovers every row');
is(sql(q{SELECT count(*) FROM c WHERE a = 0}), '2',
	'and the blocks the page maps');

# Outside recovery, map page 2 of zeroes (a lost write) fails a prefetch
# through it.  pg_prewarm's prefetch mode reaches the map only through
# bmapprefetch().
$node->stop;
open($fh, '+<:raw', $mappath) or die "could not open $mappath: $!";
seek($fh, 2 * $bs, 0);
print $fh "\0" x $bs;
close($fh);
$node->start;
sql('CREATE EXTENSION pg_prewarm');
($ret, $stdout, $stderr) =
  $node->psql('postgres', q{SELECT pg_prewarm('c', 'prefetch')});
isnt($ret, 0, 'outside recovery, a prefetch through a zeroed map page fails');
like(
	$stderr,
	qr/block 2 of ".*_bmap" is not an initialized block map page/,
	'as a map page that isn\'t initialized');

$node->stop;
done_testing();
