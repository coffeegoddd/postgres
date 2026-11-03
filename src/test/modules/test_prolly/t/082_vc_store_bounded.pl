
# Copyright (c) 2026, PostgreSQL Global Development Group

# The history store stays the window's size however long history grows:
# commits each in a transaction of its own, a window of ten commits
# severed and its segment discarded every ten, and what that lets go of
# freed by VACUUM's freer, as in a server's life.  The chunks the store
# holds at 1,000 commits are those it held at 100.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('vc_bounded');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	SELECT test_vc_open();
	SELECT test_vc_commit('main', 'base');
});

# (n more commits, each setting one of 100 rows in turn, so the table
# stays the same size; the window severed every ten, its cut discarded)
my $commits = sub {
	my ($n) = @_;
	my $sql = '';
	for my $i (1 .. $n)
	{
		$sql .= "SELECT test_vc_bench_commits('main', 1, 100);\n";
		$sql .= "SELECT test_vc_sever(10, true);\n" if $i % 10 == 0;
	}
	$node->safe_psql('postgres', $sql);
};

# the chunks the store holds once what nothing holds is freed
# (what was let go of freed first, then no freer still running: a freer
# VACUUM asks for may not have started when it returns)
my $settled = sub {
	$node->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
	ok( $node->poll_query_until(
			'postgres',
			q{SELECT unheld = 0 AND test_chunk_index_marked('pg_history.chunk_index') = 0
			FROM test_cs_check('pages')}),
		'what nothing holds freed');
	ok( $node->poll_query_until(
			'postgres',
			q{SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'chunk freer'}),
		'the freer done');
	my ($ok, $chunks, $unheld) = split /\|/,
	  $node->safe_psql('postgres', q{SELECT ok, chunks, unheld FROM test_cs_check('pages')});
	is($ok, 't', 'the store checks');
	is($unheld, '0', 'nothing unheld left');
	return $chunks;
};

$commits->(100);
my $at100 = $settled->();
$commits->(900);
my $at1000 = $settled->();
cmp_ok($at1000, '<=', $at100 + $at100 / 10,
	"the store at 1,000 commits ($at1000 chunks) is its size at 100 ($at100)");
is( $node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'vc/commit/%'}),
	'10',
	'ten commits held');

# and without the window's severs, history is all held: the measure sees it
$node->safe_psql('postgres',
	join('', map { "SELECT test_vc_bench_commits('main', 1, 100);\n" } 1 .. 200));
cmp_ok($settled->(), '>', $at1000 + 100, 'two hundred commits kept are more');

done_testing();
