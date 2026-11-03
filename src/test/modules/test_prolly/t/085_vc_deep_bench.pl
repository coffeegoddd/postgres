
# Copyright (c) 2026, PostgreSQL Global Development Group

# The deep history bench's functions do what the bench reports: commits at
# random, a sever that cuts nothing, and a probe that checks what it finds
# (the near and far merge bases, the line's length, what each diff changed).
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('vc_deep_bench');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	SELECT test_vc_open();
	SELECT test_vc_bench_load('main', 2000, 1000);
	SELECT test_vc_branch('old', 'main');
	SELECT test_vc_commit('old', 'old');
});
ok($node->safe_psql('postgres', q{SELECT test_vc_bench_deep_commits('main', 1200, 4, 2000)}) > 0,
	'commits timed');
ok($node->safe_psql('postgres', q{SELECT test_vc_bench_sever(2000)}) > 0, 'a sever timed');
my ($ret, $out, $err) = $node->psql('postgres', q{SELECT test_vc_bench_sever(2)});
isnt($ret, 0, 'a sever that cuts is refused');
like($err, qr/cut commits/, '(by name)');
# (held: the 1,200 commits, the load and the first)
is( $node->safe_psql(
		'postgres',
		q{SELECT held, diff1_rows = 4, diff1k_rows > 4, diffhalf_rows > diff1_rows,
			log100_us > 0, mb_short_us > 0, mb_long_ms > 0
		FROM test_vc_bench_deep_probe('main', 'old')}),
	'1202|t|t|t|t|t|t',
	'the probe, its merge bases checked, its diffs as far apart as asked');
$node->stop;
done_testing();
