# Copyright (c) 2026, PostgreSQL Global Development Group

# Commits from many backends at once.  Each branch is a row of
# pg_history.pointers that a commit, and an edit of its working set, lock
# before they read it, so commits on one branch take turns and none, nor
# its change, is lost; commits on different branches change different
# rows (and chunks) and never wait on each other.
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
log_lock_waits = on
deadlock_timeout = 10ms
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE SEQUENCE n;
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

# sixteen backends, a branch each: none waits on another
my $log_at = -s $node->logfile;
$node->pgbench(
	'--no-vacuum --client=16 --jobs=4 --transactions=25',
	0,
	[qr{processed: 400/400}],
	[qr{^$}],
	'sixteen backends commit on branches of their own',
	{
		'077_own_branch' => q{
SELECT test_vc_commit('b' || (:client_id + 1), 'b' || (:client_id + 1) || '-' || nextval('n'));
}
	});
my @lost = grep { $commits->("b$_") != 27 } 1 .. 16;
is(scalar @lost, 0, 'each branch has its 25 commits, on base and init')
  or diag("branches short: @lost");
# (each commit's row in its tree: none's change lost to another's)
my @rows = grep {
	$node->safe_psql('postgres', qq{SELECT test_vc_rows('b$_')}) != 26
} 1 .. 16;
is(scalar @rows, 0, "each branch's head has its 25 commits' rows, and base's")
  or diag("branches short of rows: @rows");
my $waits = slurp_file($node->logfile, $log_at);
unlike($waits, qr/still waiting/, 'no commit waited on another branch\'s');

# eight backends on one branch: they take turns, and every commit lands
$node->pgbench(
	'--no-vacuum --client=8 --jobs=4 --transactions=25',
	0,
	[qr{processed: 200/200}],
	[qr{^$}],
	'eight backends commit on one branch',
	{
		'077_one_branch' => q{
SELECT test_vc_commit('main', 'm' || nextval('n'));
}
	});
is($commits->('main'), 202, "main has all 200 commits, on base and init");
is($node->safe_psql('postgres', q{SELECT test_vc_rows('main')}),
	'201', "main's head has every commit's row: none's change lost");
is($node->safe_psql('postgres', q{SELECT test_vc_head_in_window('main')}),
	't', "main's head keeps its row");

# every commit kept has its row: 400 + 200, base and init
is( $node->safe_psql('postgres',
		q{SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'vc/commit/%'}),
	'602', 'every commit has its row');

$node->stop;
done_testing();
