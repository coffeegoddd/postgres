# Copyright (c) 2026, PostgreSQL Global Development Group

# Crash recovery of a truncated relation whose map's header reached disk
# after the truncation.  Recovery reads the header's length as it is on
# disk: shorter than the relation was when the records before the
# truncation were made.  A record that restores or initializes a page past
# that length extends the relation in recovery, as md.c's file is extended
# when it was truncated before the crash; the truncation's replay removes
# the page again.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('past_length');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
shared_buffers = 400kB
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# The map's header written out, as the background writer or an eviction
# may write it (if it isn't already).
sub evict_header
{
	my $rel = shift;
	return sql(
		qq{SELECT coalesce(bool_and((pg_buffercache_evict(bufferid)).buffer_evicted), true)
		FROM pg_buffercache
		WHERE relfilenode = pg_relation_filenode('$rel')
		  AND relforknumber = 4 AND relblocknumber = 0});
}

sub nblocks
{
	my $rel = shift;
	return sql(
		qq{SELECT pg_relation_size('$rel') / current_setting('block_size')::int});
}

sql(q{CREATE EXTENSION pg_buffercache});

# Crash and recover, the truncation's records replayed after the pages'.
sub crash_and_recover
{
	$node->stop('immediate');
	my $log_offset = -s $node->logfile;
	ok($node->start(fail_ok => 1), 'recovery completes');
	ok( !$node->log_contains('extended during recovery', $log_offset),
		'without refusing the extension');
}

# A VACUUM's truncation: the pages it cuts off had images logged after the
# checkpoint recovery starts at (hint bits the scan set).
sql(q{CREATE TABLE h (a int, b text);
	INSERT INTO h SELECT g, repeat('x', 500) FROM generate_series(1, 1500) g});
is(nblocks('h'), '100', 'the table has 100 blocks');
sql('CHECKPOINT');
sql(q{DELETE FROM h WHERE a > 150});
sql(q{VACUUM h});
is(nblocks('h'), '10', 'the VACUUM truncates it to 10');
is(evict_header('h'), 't', 'the map\'s header is written out');

crash_and_recover();
is(nblocks('h'), '10', 'the table has the length it was truncated to');
is(sql(q{SELECT count(*) FROM h}), '150', 'and its rows');

# extending again after recovery reuses the blocks past the length
sql(q{INSERT INTO h SELECT g, repeat('y', 500) FROM generate_series(1, 300) g});
is(sql(q{SELECT count(*) FROM h}), '450', 'the table extends again');
is(nblocks('h'), '30', 'over the blocks it had');

# Pages past the length that records change again after their images: they
# are read at the length recovery keeps for the relation past its header's,
# until the truncation's replay.
sql(q{CREATE TABLE g (a int, b text);
	INSERT INTO g SELECT g, repeat('x', 500) FROM generate_series(1, 300) g});
sql('CHECKPOINT');
sql(q{UPDATE g SET b = 'y' WHERE a > 285});
sql(q{UPDATE g SET b = 'z' WHERE a > 285});
sql(q{DELETE FROM g WHERE a > 15});
sql(q{VACUUM g});
is(nblocks('g'), '1', 'the second table is truncated to 1 block');
is(evict_header('g'), 't', 'and its map\'s header written out');

crash_and_recover();
is(nblocks('g'), '1', 'the second table has its length');
is(sql(q{SELECT count(*) FROM g}), '15', 'and its rows');
sql(q{INSERT INTO g SELECT g, repeat('y', 500) FROM generate_series(1, 30) g});
is(sql(q{SELECT count(*) FROM g}), '45', 'and extends again');

$node->stop;
done_testing();
