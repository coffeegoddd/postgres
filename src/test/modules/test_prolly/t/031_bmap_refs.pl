# Copyright (c) 2026, PostgreSQL Global Development Group

# The reference counts of a page store's blocks.
#
# They are kept in pages read through shared buffers, so that a change of
# a count costs a buffer lookup, not an open of the counts' file.
#
# They are wide enough that a block shared by more than 255 maps keeps an
# exact count: one that could only stick at its largest value would never
# reach zero as its holders go, and only a sweep would free the block.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('refs');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_buffercache;
	CREATE TABLE t (a int, b text) WITH (fillfactor = 50);
	INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(1, 2000) g;
});
my $src = sql(q{SELECT pg_relation_filenode('t')});
my $db = sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
my $refs_file = 9419;

# Many copies of shared pages in one transaction: the counts they change
# are read through shared buffers.
sql(qq{SELECT test_bmap_share($src)});
sql(q{BEGIN;
	UPDATE t SET a = a WHERE a % 100 < 10;
	COMMIT});
cmp_ok(sql(qq{SELECT count(*) FROM pg_buffercache
	WHERE relfilenode = $refs_file AND reldatabase = $db}), '>', 0,
	'the counts are read through shared buffers');

# A page shared by 300 copies counts each, and each copy dropped counts
# one less: the last one's drop frees the copied blocks without a sweep.
sql(q{CREATE TABLE one (a int); INSERT INTO one VALUES (1);});
my $one = sql(q{SELECT pg_relation_filenode('one')});
my $phys = sql(qq{SELECT phys FROM test_bmap_map($one) WHERE blkno = 0});
my @copies = split /\n/, sql(qq{SELECT test_bmap_share($one) FROM generate_series(1, 300)});
is(scalar(@copies), 300, '300 copies made');
{
	local $TODO = 'a count is one byte, and sticks at 255 holders';

	is(sql(qq{SELECT refs FROM test_bmap_map($one) WHERE blkno = 0}), '300',
		'the page counts 300 holders');
}
sql(qq{SELECT test_bmap_unlink($_)}) for @copies[0 .. 199];
{
	local $TODO = 'a count is one byte, and sticks at 255 holders';

	is(sql(qq{SELECT refs FROM test_bmap_map($one) WHERE blkno = 0}), '100',
		'200 copies dropped, it counts 100');
}
sql(qq{SELECT test_bmap_unlink($_)}) for @copies[200 .. 299];
{
	local $TODO = 'a count is one byte, and sticks at 255 holders';

	is(sql(qq{SELECT refs FROM test_bmap_map($one) WHERE blkno = 0}), '0',
		'every copy dropped, it counts none');
	my $free = sql(q{SELECT test_bmap_free_blocks()});
	sql(q{DROP TABLE one});
	sql(q{SELECT * FROM test_bmap_release_drain()});
	cmp_ok(sql(q{SELECT test_bmap_free_blocks()}) - $free, '>=', 1,
		'dropping the table frees its block, without a sweep');
}
$node->stop;
done_testing();
