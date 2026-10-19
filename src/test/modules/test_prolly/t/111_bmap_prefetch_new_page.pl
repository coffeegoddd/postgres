# Copyright (c) 2026, PostgreSQL Global Development Group

# Crash recovery's prefetch of a block whose map page is still zeroes on
# disk.  An extension that adds a map page logs the page and the map's
# header in one record, and either can reach disk first; with the header
# written and the new page not, the page is zeroes on disk, below the map's
# length, until the record is replayed.  The WAL prefetcher looks blocks up
# ahead of replay, and a lookup of a block on that page finds no entry: the
# prefetch is skipped, not a failure of recovery.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('prefetch');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
recovery_prefetch = try
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION pg_buffercache;
	CREATE TABLE c (a int, b text);
	INSERT INTO c SELECT g, repeat('x', 1000) FROM generate_series(1, 3570) g});
is(sql(q{SELECT pg_relation_size('c') / current_setting('block_size')::int}),
	'510', 'the table has blocks 0..509, the whole of map page 1');

# Block 510 adds map page 2, after the checkpoint recovery starts at; the
# changes of the block after it are what the prefetcher looks up ahead.
sql('CHECKPOINT');
sql(q{INSERT INTO c SELECT g, repeat('y', 1000) FROM generate_series(1, 7) g});
sql(q{INSERT INTO c VALUES (0, 'a')});
sql(q{INSERT INTO c VALUES (0, 'b')});

# The map's header reaches disk; map page 2 doesn't.
is( sql(q{SELECT bool_and((pg_buffercache_evict(bufferid)).buffer_evicted) FROM pg_buffercache
		WHERE relfilenode = pg_relation_filenode('c')
		  AND relforknumber = 4 AND relblocknumber = 0}),
	't',
	'the map\'s header is written out');
my $bs = sql(q{SELECT current_setting('block_size')});
my $mappath = $node->data_dir . '/' . sql(q{SELECT pg_relation_filepath('c')}) . '_bmap';
open(my $fh, '<:raw', $mappath) or die "could not open $mappath: $!";
seek($fh, 2 * $bs, 0);
my $page;
is(read($fh, $page, $bs), $bs, 'map page 2 is in the file');
close($fh);
is($page, "\0" x $bs, 'and is zeroes on disk');

$node->stop('immediate');
my $log_offset = -s $node->logfile;
$node->start(fail_ok => 1);
ok(!$node->log_contains('is not an initialized block map page', $log_offset),
	'recovery prefetches past the zeroed map page');
is(sql(q{SELECT count(*) FROM c}), '3579', 'and recovers every row');
is(sql(q{SELECT count(*) FROM c WHERE a = 0}), '2',
	'the changes of the block the page maps too');

$node->stop;
done_testing();
