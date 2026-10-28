# Copyright (c) 2026, PostgreSQL Global Development Group

# A hard reset to a commit kept with a root, from a large working set the
# fold worker is still folding, gives the tables their kept pages and
# writes no row.  The worker's fold holds the tables' locks to its end, and
# the reset only tries them; so the reset asks the fold to cancel, as it
# asks an autovacuum worker, and the fold gives up, rolled back, and is
# made again later.  Counted in restores_fold_cancels; nothing given up on
# (restores_lock_busy).  Afterwards the rows are the commit's, the working
# set is clean, and a later commit holds what was written since.
#
# The fold is held mid-fold, the table locked, at an injection point, on
# the branch the commits were made on (a restore of its own storage) and
# on another branch made of its head (a share of the kept pages).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('reset_fold_holder');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = on
prolly_fold_worker_naptime = 50
fsync = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{ALTER DATABASE postgres SET prolly.history_window = '100 years';
	ALTER DATABASE postgres SET prolly.history_min_commits = 50;
	ALTER DATABASE postgres SET prolly.root_interval = 0;
	ALTER DATABASE postgres SET prolly.root_interval_commits = 1});
sql(q{CREATE EXTENSION injection_points;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, rev int NOT NULL, payload text NOT NULL);
	INSERT INTO app.t SELECT g, 0, repeat(md5(g::text), 4)
	  FROM generate_series(1, 20000) g;
	SELECT dolt_commit('-Am', 'load') IS NOT NULL});

my $digest = q{SELECT md5(string_agg(id || ':' || rev, ',' ORDER BY id)) FROM app.t};
my @commits;
my %at;
for my $r (1 .. 8)
{
	my $h = sql(qq{UPDATE app.t SET rev = $r WHERE id % 100 = $r;
		SELECT (dolt_commit('-am', 'r$r'))[1]});
	push @commits, $h;
	$at{$h} = sql($digest);
}
my $target = $commits[2];
sql(q{SELECT dolt_branch('scratch')});

my @names = qw(restores_lock_busy restores_fold_cancels move_tables_written
  apply_rows moves_root_kept relations_restored move_tables_shared
  fold_worker_retries);
my $names = join(', ', map { "'$_'" } @names);
sub counters
{
	my %c = split /[|\n]/,
	  sql(qq{SELECT name, value FROM pg_stat_prolly WHERE name IN ($names)});
	$c{$_} //= 0 for @names;
	return \%c;
}

# the fold worker, folding branch $branch's working set, stopped holding
# app.t; then a hard reset of the branch to $target from a session of its
# own: what it counted
sub reset_beside_fold
{
	my ($branch) = @_;
	my $co = "SELECT dolt_checkout('$branch') IS NOT NULL;";

	sql(q{SELECT injection_points_attach('prolly-fold-table', 'wait', 'worker')});
	sql(qq{$co UPDATE app.t SET rev = 99 WHERE id <= 18000});
	ok( $node->poll_query_until(
			'postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
			WHERE backend_type = 'prolly fold worker'
			  AND wait_event = 'prolly-fold-table'}),
		"$branch: the fold worker is stopped mid-fold");
	my $worker = sql(q{SELECT pid FROM pg_stat_activity
		WHERE backend_type = 'prolly fold worker'
		  AND wait_event = 'prolly-fold-table'});
	ok( sql(qq{SELECT count(*) > 0 FROM pg_locks WHERE pid = $worker
			AND locktype = 'relation' AND relation = 'app.t'::regclass
			AND granted}),
		"$branch: holding app.t");

	my $before = counters();
	my $out = sql(qq{$co SELECT 'reset ' || (dolt_reset('--hard', '$target') IS NOT NULL);
		SELECT pg_stat_force_next_flush()});
	sql(q{SELECT injection_points_detach('prolly-fold-table')});
	# (none waits if the worker's retry didn't get there)
	$node->psql('postgres', q{SELECT injection_points_wakeup('prolly-fold-table')});
	like($out, qr/reset t/, "$branch: the reset finished");
	my $after = counters();
	my %d = map { $_ => $after->{$_} - $before->{$_} } @names;
	$d{retries_before} = $before->{fold_worker_retries};
	return \%d;
}

for my $branch ('main', 'scratch')
{
	my $co = "SELECT dolt_checkout('$branch') IS NOT NULL;";
	my $d = reset_beside_fold($branch);

	is($d->{restores_lock_busy}, 0, "$branch: no restore given up on");
	is($d->{move_tables_written}, 0, "$branch: no table written");
	is($d->{apply_rows}, 0, "$branch: no row written");
	is($d->{moves_root_kept}, 1, "$branch: the move found the kept root");
	ok($d->{relations_restored} + $d->{move_tables_shared} > 0,
		"$branch: the tables were given their kept pages");
	ok($d->{restores_fold_cancels} >= 1, "$branch: the fold was asked to cancel");
	my @rows = split /\n/, sql(qq{$co $digest});
	is($rows[-1], $at{$target}, "$branch: the rows are the commit's");
	@rows = split /\n/, sql(qq{$co SELECT count(*) FROM dolt_status});
	is($rows[-1], '0', "$branch: the working set is clean");

	# (the cancelled fold given up quietly, and made again)
	ok( $node->poll_query_until(
			'postgres', qq{SELECT value > $d->{retries_before} FROM pg_stat_prolly
			WHERE name = 'fold_worker_retries'}),
		"$branch: the fold worker went on");
	unlike(slurp_file($node->logfile), qr/canceling statement/,
		"$branch: the cancelled fold reported no error");

	# a later change, committed
	sql(qq{$co UPDATE app.t SET rev = 7 WHERE id = 1});
	@rows = split /\n/, sql(qq{$co SELECT (dolt_commit('-am', 'after $branch'))[1] IS NOT NULL;
		SELECT count(*) FROM dolt_status;
		SELECT rev FROM app.t WHERE id = 1;
		SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::app.t)});
	is(join(',', @rows[-3 .. -1]), '0,7,1',
		"$branch: a later commit holds the change made since, alone");
}

ok(sql(q{SELECT count(*) FROM pg_stat_activity
	WHERE backend_type = 'prolly fold worker' AND wait_event = 'prolly-fold-table'}) == 0,
	'no fold left stopped');

$node->stop;
done_testing();
