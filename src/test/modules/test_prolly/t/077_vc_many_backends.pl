# Copyright (c) 2026, PostgreSQL Global Development Group

# Commits from many backends at once.  Each branch is a row of
# pg_history.pointers that a commit locks before it reads it, so commits
# on one branch take turns and none is lost; commits on different branches
# change different rows (and chunks) and never wait on each other.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('vc_many');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
max_connections = 40
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	SELECT test_vc_open();
	SELECT test_vc_commit('main', 'base');
	SELECT test_vc_branch('b' || i, 'main') FROM generate_series(1, 16) i;
});

# the commits on a branch's first-parent line, base and init with them
my $commits = sub {
	my ($branch) = @_;
	return $node->safe_psql('postgres',
		qq{SELECT cardinality(string_to_array(test_vc_log('$branch'), ' '))});
};

# sixteen backends, a branch each
$node->pgbench(
	'--no-vacuum --client=16 --jobs=4 --transactions=25',
	0,
	[qr{processed: 400/400}],
	[qr{^$}],
	'sixteen backends commit on branches of their own',
	{
		'077_own_branch' => q{
\set n random(1, 1000000000)
SELECT test_vc_commit('b' || (:client_id + 1), 'b' || (:client_id + 1) || '-' || :n);
}
	});
my @lost = grep { $commits->("b$_") != 27 } 1 .. 16;
is(scalar @lost, 0, 'each branch has its 25 commits, on base and init')
  or diag("branches short: @lost");

# eight backends on one branch: they take turns, and every commit lands
$node->pgbench(
	'--no-vacuum --client=8 --jobs=4 --transactions=25',
	0,
	[qr{processed: 200/200}],
	[qr{^$}],
	'eight backends commit on one branch',
	{
		'077_one_branch' => q{
\set n random(1, 1000000000)
SELECT test_vc_commit('main', 'm' || :client_id || '-' || :n);
}
	});
is($commits->('main'), 202, "main has all 200 commits, on base and init");
is($node->safe_psql('postgres', q{SELECT test_vc_head_in_window('main')}),
	't', "main's head keeps its row");

# every commit kept has its row: 400 + 200, base and init
is( $node->safe_psql('postgres',
		q{SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'vc/commit/%'}),
	'602', 'every commit has its row');

$node->stop;
done_testing();
