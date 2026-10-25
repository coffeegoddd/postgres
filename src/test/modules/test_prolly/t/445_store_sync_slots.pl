# Copyright (c) 2026, PostgreSQL Global Development Group

# Shared memory follows the syncs of at most 64 page stores (a database's
# in a tablespace) for skipping the sync of a share or snapshot that holds
# nothing new (#947).  Past that, the least recently used idle store's slot
# is let go of: a store used later still skips its syncs once synced, and
# one whose slot was let go of syncs again first, as one not known durable.

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

# In database 'db': a snapshot with blocks of its own, then one with none:
# the store syncs each ran.
sub snapshots
{
	my ($db, $setup) = @_;
	return $node->safe_psql($db, qq{
		$setup
		SET debug_block_mapped_heap = on;
		CREATE TEMP TABLE before AS SELECT * FROM test_prolly_session_stats();
		UPDATE plain SET a = a + 1 WHERE a % 10 = 0;
		SELECT test_bmap_snapshot_take(pg_relation_filenode('plain'));
		CREATE TEMP TABLE mid AS SELECT * FROM test_prolly_session_stats();
		SELECT test_bmap_snapshot_take(pg_relation_filenode('plain'));
		SELECT (m.value - b.value) || ',' || (s.value - m.value)
		FROM test_prolly_session_stats() s JOIN mid m USING (name)
			JOIN before b USING (name)
		WHERE s.name = 'bmap_store_syncs';
	}) =~ s/^.*\n//sr;
}

my @got;
for my $i (1 .. $stores)
{
	$node->safe_psql('postgres', "CREATE DATABASE s$i");
	push @got, snapshots("s$i", q{
		CREATE EXTENSION test_prolly;
		SET debug_block_mapped_heap = on;
		CREATE TABLE plain (a int, b text);
		INSERT INTO plain SELECT g, repeat('p', 100) FROM generate_series(1, 500) g;});
}
is_deeply(\@got, [ ('1,0') x $stores ],
	"in each of $stores stores, a snapshot with nothing new skips its sync");

# Two snapshots with nothing new: the store syncs each ran.
sub unchanged
{
	my ($db) = @_;
	return $node->safe_psql($db, q{
		CREATE TEMP TABLE before AS SELECT * FROM test_prolly_session_stats();
		SELECT test_bmap_snapshot_take(pg_relation_filenode('plain'));
		CREATE TEMP TABLE mid AS SELECT * FROM test_prolly_session_stats();
		SELECT test_bmap_snapshot_take(pg_relation_filenode('plain'));
		SELECT (m.value - b.value) || ',' || (s.value - m.value)
		FROM test_prolly_session_stats() s JOIN mid m USING (name)
			JOIN before b USING (name)
		WHERE s.name = 'bmap_store_syncs';
	}) =~ s/^.*\n//sr;
}

# The first store's slot was let go of: its next snapshot with nothing new
# syncs (the store not known durable), and the one after skips.  The last
# store's slot is still there.
is(unchanged('s1'), '1,0', 'a store whose slot was let go of syncs, then skips again');
is(unchanged("s$stores"), '0,0', 'a store that kept its slot skips both');

$node->stop;
done_testing();
