# Copyright (c) 2026, PostgreSQL Global Development Group

# Shared memory follows the flushes of shares' and captures' own blocks of
# at most 64 page stores (a database's in a tablespace), so that a share
# holding nothing new skips its flush when none is in flight.  Past that,
# the least recently used idle store's slot is let go of: a store used
# later takes a slot again, counting its flushes from there.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('store_sync_slots');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off");
$node->start;

my $stores = 70;

# In database 'db': a capture with blocks of its own, then one with none;
# then the store's slot: its flushes written, and in flight ('none' if it
# has no slot).
sub captures
{
	my ($db, $setup) = @_;
	return $node->safe_psql($db, qq{
		$setup
		SET debug_block_mapped_heap = on;
		UPDATE plain SET a = a + 10 WHERE a % 10 = 0;
		SELECT test_bmap_root_capture(pg_relation_filenode('plain'));
		SELECT test_bmap_root_capture(pg_relation_filenode('plain'));
		SELECT coalesce(written || ',' || flushing, 'none') FROM test_bmap_store_sync();
	}) =~ s/^.*\n//sr;
}

sub slot_of
{
	my ($db) = @_;
	return $node->safe_psql($db, q{
		SELECT coalesce(written || ',' || flushing, 'none') FROM test_bmap_store_sync()});
}

my @got;
for my $i (1 .. $stores)
{
	$node->safe_psql('postgres', "CREATE DATABASE s$i");
	push @got, captures("s$i", q{
		CREATE EXTENSION test_prolly;
		SET debug_block_mapped_heap = on;
		CREATE TABLE plain (a int, b text);
		INSERT INTO plain SELECT g, repeat('p', 100) FROM generate_series(1, 500) g;});
}
is_deeply(\@got, [ ('1,0') x $stores ],
	"in each of $stores stores, the capture with blocks of its own counts one flush written, the other none");

# The first stores' slots were let go of for the last ones; the last
# store's slot is still there.
is(slot_of('s1'), 'none', 'the least recently used store\'s slot was let go of');
is(slot_of("s$stores"), '1,0', 'a store used since keeps its slot');
is(captures('s1', ''), '1,0', 'a store whose slot was let go of takes one again');

$node->stop;
done_testing();
