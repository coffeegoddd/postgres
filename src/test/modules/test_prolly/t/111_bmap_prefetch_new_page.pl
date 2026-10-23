# Copyright (c) 2026, PostgreSQL Global Development Group

# Crash recovery's prefetch of a block whose leaf is still zeroes on disk.
# An extension that adds a leaf (and the node above it) logs each node with
# the pointer to it, the root page's or a node's, in one record, and either
# can reach disk first; with the root page written and the new nodes not,
# they are zeroes on disk, under the map's length, until the records are
# replayed.  The WAL prefetcher looks blocks up ahead of replay, and a
# lookup of a block under those nodes finds no entry (the node isn't the
# one its pointer names): the prefetch is skipped, not a failure of
# recovery.  Outside recovery, a leaf of zeroes is a lost write, and a
# prefetch through it fails.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Nothing writes out a dirty buffer but the test's evictions: the new
# nodes stay zeroes on disk until the crash.
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
# leaf has entries: (block size - page header - special space) / entry.
my $bs = sql(q{SELECT current_setting('block_size')});
my $fanout = int(($bs - 24 - 24) / 16);
my $half = $bs / 2;
sql(qq{CREATE EXTENSION pg_buffercache; CREATE EXTENSION test_prolly;
	CREATE TABLE c (a int, b text);
	ALTER TABLE c ALTER b SET STORAGE PLAIN;
	INSERT INTO c SELECT g, repeat('x', $half) FROM generate_series(1, $fanout) g});
is(sql(q{SELECT pg_relation_size('c') / current_setting('block_size')::int}),
	$fanout, 'the table has the whole of its first leaf');

# The next block adds the next subtree's node and leaf, after the
# checkpoint recovery starts at.
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
	'the rows on the new leaf\'s blocks are locked');

# The map's header and root page reach disk; the new nodes don't.
ok( $node->poll_query_until(
		'postgres', q{SELECT bool_and((pg_buffercache_evict(bufferid)).buffer_evicted) FROM pg_buffercache
		WHERE relfilenode = pg_relation_filenode('c')
		  AND relforknumber = 4}),
	'the map\'s header and root page are written out');
my $leaf = sql(qq{SELECT block FROM test_bmap_tree(pg_relation_filenode('c'))
	WHERE level = 0 AND first = $fanout});
my $storepath = $node->data_dir . '/base/' . sql(q{SELECT oid FROM pg_database WHERE datname = current_database()})
  . '/9421';

$node->stop('immediate');
open(my $fh, '<:raw', $storepath) or die "could not open $storepath: $!";
seek($fh, $leaf * $bs, 0);
my $page;
is(read($fh, $page, $bs), $bs, 'the new leaf is in the page store');
close($fh);
is($page, "\0" x $bs, 'and is zeroes on disk at the crash');

my $log_offset = -s $node->logfile;
$node->start(fail_ok => 1);
ok(!$node->log_contains('not the node it names', $log_offset),
	'recovery prefetches past the zeroed leaf');
is(sql(q{SELECT count(*) FROM c}), $fanout + 2, 'and recovers every row');
is(sql(q{SELECT count(*) FROM c WHERE a = 0}), '2',
	'and the blocks the page maps');

# Outside recovery, a leaf of zeroes (a lost write) fails a prefetch
# through it.  pg_prewarm's prefetch mode reaches the map only through
# bmapprefetch().
$leaf = sql(qq{SELECT block FROM test_bmap_tree(pg_relation_filenode('c'))
	WHERE level = 0 AND first = $fanout});
$node->stop;
open($fh, '+<:raw', $storepath) or die "could not open $storepath: $!";
seek($fh, $leaf * $bs, 0);
print $fh "\0" x $bs;
close($fh);
$node->start;
sql('CREATE EXTENSION pg_prewarm');
($ret, $stdout, $stderr) =
  $node->psql('postgres', q{SELECT pg_prewarm('c', 'prefetch')});
isnt($ret, 0, 'outside recovery, a prefetch through a zeroed leaf fails');
like(
	$stderr,
	qr/points to block $leaf of its page store, which is not the node it names/,
	'as a node that isn\'t the one named');

$node->stop;
done_testing();
