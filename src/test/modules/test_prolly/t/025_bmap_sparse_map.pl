# Copyright (c) 2026, PostgreSQL Global Development Group

# The largest relations work: a block map reaching the last block a
# relation can have, 2^32 - 2, built sparsely, so that a relation extended
# far past its end (as a hash index's split does) costs the blocks it
# writes, not one for each block below its new end.  The map's tree has
# the levels to name such a block; what is missing is a map with holes:
# extending a block-mapped relation gives every block below the new end a
# physical block and a map entry, which for 2^32 blocks is 32 TB of store
# and tens of gigabytes of map.  So the test is skipped until extension
# leaves the blocks between unmapped (read as zeroes).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all =>
  'block-mapped storage maps every block below the end it extends to, so a map with holes up to 2^32 blocks cannot be built';

my $node = PostgreSQL::Test::Cluster->new('sparse_map');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE t (a int);
	INSERT INTO t VALUES (1);
});
my $t = sql(q{SELECT pg_relation_filenode('t')});

# Extended to the last block a relation can have: the blocks between get
# no physical block, and the last one does.
sql(qq{SELECT test_bmap_extend($t, 2147483647)});
sql(qq{SELECT test_bmap_extend($t, 2147483646)});
is(sql(qq{SELECT pg_relation_size('t') / 8192}), '4294967294',
	'the relation reaches the last block a relation can have');
cmp_ok(sql(qq{SELECT count(*) FROM test_bmap_map($t)}), '<=', 2,
	'with a physical block only for the blocks written');
sql(qq{SELECT test_bmap_write($t, -2, 7)});
is(sql(q{SELECT count(*) FROM t}), '1', 'the table reads its row still');

$node->stop('immediate');
$node->start;
is(sql(qq{SELECT pg_relation_size('t') / 8192}), '4294967294',
	'crash recovery keeps the length');

$node->stop;
done_testing();
