# Copyright (c) 2026, PostgreSQL Global Development Group

# Commits from many backends at once.  Each branch is a row of
# pg_history.pointers that a commit, and an edit of its working set, lock
# before they read it, so commits on one branch take turns and none, nor
# its change, is lost; commits on different branches change different
# rows (and chunks) and never wait for each other's transactions, nor
# deadlock (only, briefly, for one extending a relation both grow).
#
# A backend's first write takes a chunk-log lane, making one if none is
# free; a new lane is seen as its maker commits, a moment before the
# maker lets go of its lock on it, so one opening it then waits that
# moment.  The lanes are made first, by a run before the one measured.
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
deadlock_timeout = 1ms
log_line_prefix = '%m [%p] %b %a: '
log_min_error_statement = log
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

# the lanes made: as many backends as below commit once each first
$node->pgbench(
	'--no-vacuum --client=16 --jobs=4 --transactions=1',
	0,
	[qr{processed: 16/16}],
	[qr{^$}],
	'sixteen backends make their lanes',
	{
		'077_lanes' => q{
SELECT test_vc_commit('b' || (:client_id + 1), 'b' || (:client_id + 1) || '-lane');
}
	});

# sixteen backends, a branch each: none waits for another's transaction
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
my @lost = grep { $commits->("b$_") != 28 } 1 .. 16;
is(scalar @lost, 0, 'each branch has its 25 commits, on its lane commit, base and init')
  or diag("branches short: @lost");
# (each commit's row in its tree: none's change lost to another's)
my @rows = grep {
	$node->safe_psql('postgres', qq{SELECT test_vc_rows('b$_')}) != 27
} 1 .. 16;
is(scalar @rows, 0, "each branch's head has its 26 commits' rows, and base's")
  or diag("branches short of rows: @rows");
# (a wait's lines, with what waited and on what, shown if there is one)
my $waits = slurp_file($node->logfile, $log_at);
my @waited = grep { /still waiting/ ... /STATEMENT|^\S+ \S+ \S+ \[\d+\] .*: (?!DETAIL|CONTEXT)/ }
  split /\n/, $waits;
# (each relation a wait was on, named: its oid differs between builds)
my %rels = map { $_ => 1 } ($waits =~ /relation (\d+) of database/g);
my @named = map {
	"relation $_ is "
	  . $node->safe_psql('postgres',
		"SELECT coalesce((SELECT oid::regclass::text FROM pg_class WHERE oid = $_), 'gone')")
} sort keys %rels;
# (a commit may wait briefly for another to extend a relation they both
# grow, a page of the chunk index or the pointers; never for another's
# transaction to end, nor for a lock on a row or relation, nor deadlock)
my @long = grep { /still waiting/ && !/on extension of relation/ } @waited;
is(scalar @long, 0, 'no commit waited for another branch\'s during the 16-branch run')
  or diag(join("\n", @named, @waited));
unlike($waits, qr/deadlock detected/, 'no deadlock during the 16-branch run');

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

# every commit kept has its row: 16 + 400 + 200, base and init
is( $node->safe_psql('postgres',
		q{SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'vc/commit/%'}),
	'618', 'every commit has its row');

$node->stop;
done_testing();
