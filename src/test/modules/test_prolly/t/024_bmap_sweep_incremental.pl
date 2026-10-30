# Copyright (c) 2026, PostgreSQL Global Development Group

# A sweep counts what the captured roots hold.  It must count only the
# roots made or let go of since the last sweep, so that its cost doesn't
# grow with the roots held: a database keeping many roots (one for each
# commit of a version history) would otherwise pay for all of them at each
# sweep.  The buffers a sweep reads stand for its cost: one after a root
# is captured, with eleven roots held and with two hundred and two.

use strict;
use warnings FATAL => 'all';
use JSON::PP;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('sweep_incremental');
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
	CREATE TABLE t (a int, b text);
	INSERT INTO t SELECT g, repeat('x', 100) FROM generate_series(1, 2000) g;
});
my $t = sql(q{SELECT pg_relation_filenode('t')});

# Capture 'n' roots, each after a change, so that each holds a node of
# its own; sweep; then capture one more and sweep again: the buffers the
# second sweep looked up.
sub sweep_after_roots
{
	my ($n) = @_;

	for my $i (1 .. $n)
	{
		sql(qq{UPDATE t SET a = a WHERE a = $i;
			SELECT test_bmap_root_capture($t)});
	}
	sql(q{SELECT * FROM test_bmap_sweep()});
	sql(qq{UPDATE t SET a = a WHERE a = 1999;
		SELECT test_bmap_root_capture($t)});
	my $plan = decode_json(sql(q{
		EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, FORMAT JSON)
		SELECT * FROM test_bmap_sweep()}))->[0]{Plan};
	return $plan->{'Shared Hit Blocks'} + $plan->{'Shared Read Blocks'};
}

my $few = sweep_after_roots(10);
my $many = sweep_after_roots(190);
note("a sweep with 11 roots held looked up $few buffers, with 202 $many");
is(sql(q{SELECT held FROM test_bmap_roots()}), '202', 'the roots are held');
{
	local $TODO = 'a sweep counts every root held';

	cmp_ok($many, '<=', 1.5 * $few,
		'a sweep after one root is captured costs about the same with many roots held');
}

$node->stop;
done_testing();
