# Copyright (c) 2026, PostgreSQL Global Development Group

# The reference counts of a page store's blocks.
#
# They are kept in pages read through shared buffers, so that a change of
# a count costs a buffer lookup, not an open of the counts' file.
#
# They are wide enough that a block shared by a thousand maps keeps an
# exact count: one that could only stick at its largest value would never
# reach zero as its holders go, and only a sweep would free the block.  The
# counts come back exactly after a crash and on a standby, and a share that
# would take a count past the most it says fails rather than wrap.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('refs');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
});
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('refs_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub ssql { return $standby->safe_psql('postgres', $_[0]); }

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

# A page shared by a thousand copies counts each, and each copy dropped
# counts one less: the last one's drop frees the copied blocks without a
# sweep.  The counts are the same after a crash, and on a standby.
sql(q{CREATE TABLE one (a int); INSERT INTO one VALUES (1);});
my $one = sql(q{SELECT pg_relation_filenode('one')});
my @copies = split /\n/,
  sql(qq{SELECT test_bmap_share($one) FROM generate_series(1, 1000)});
is(scalar(@copies), 1000, '1000 copies made');
my $refs = qq{SELECT refs FROM test_bmap_map($one) WHERE blkno = 0};
is(sql($refs), '1000', 'the page counts 1000 holders');

# The most a count says, lowered to 1000 by a test's injection point: one
# more share fails, and leaves the count as it was.
SKIP:
{
	skip 'injection points not supported by this build', 4
	  if $ENV{enable_injection_points} ne 'yes';
	sql(q{CREATE EXTENSION injection_points});
	sql(q{SELECT injection_points_attach('bmap-refs-test-max', 'notice')});
	my ($ret, $out, $err) = $node->psql('postgres',
		qq{SELECT test_bmap_share($one, true)});
	isnt($ret, 0, 'a share past the most a count says fails');
	like($err, qr/is shared by too many maps/, 'saying the block is shared too much');
	sql(q{SELECT injection_points_detach('bmap-refs-test-max')});
	is(sql($refs), '1000', 'the count is as it was');
	my $more = sql(qq{SELECT test_bmap_share($one)});
	is(sql($refs), '1001', 'and one more share counts once the limit is up');
	sql(qq{SELECT test_bmap_unlink($more)});
}

$node->wait_for_replay_catchup($standby);
is(ssql($refs), '1000', 'a standby counts 1000 too');
$node->stop('immediate');
$node->start;
is(sql($refs), '1000', 'and so does crash recovery');

my $drop = sub {
	my $list = join(',', @_);
	sql(qq{SELECT test_bmap_unlink(c) FROM unnest(ARRAY[$list]::oid[]) c});
};
$drop->(@copies[0 .. 599]);
is(sql($refs), '400', '600 copies dropped, it counts 400');
$node->wait_for_replay_catchup($standby);
is(ssql($refs), '400', 'on a standby too');
$node->stop('immediate');
$node->start;
is(sql($refs), '400', 'and after a crash');

$drop->(@copies[600 .. 999]);
is(sql($refs), '0', 'every copy dropped, it counts none');
my $free = sql(q{SELECT test_bmap_free_blocks()});
sql(q{DROP TABLE one});
sql(q{SELECT * FROM test_bmap_release_drain()});
cmp_ok(sql(q{SELECT test_bmap_free_blocks()}) - $free, '>=', 2,
	'dropping the table frees its page and its map\'s node, without a sweep');
my $freed = sql(q{SELECT test_bmap_free_blocks()});
$node->wait_for_replay_catchup($standby);
is(ssql(q{SELECT test_bmap_free_blocks()}), $freed, 'a standby frees the same blocks');
$node->stop('immediate');
$node->start;
is(sql(q{SELECT test_bmap_free_blocks()}), $freed, 'and so does crash recovery');
$standby->stop;
$node->stop;
done_testing();
