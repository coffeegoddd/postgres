# Copyright (c) 2026, PostgreSQL Global Development Group

# A fold of the fold worker's that a hard reset asks to cancel gives up
# within its sort of the working set's changes, not after it: the sort of
# a large working set runs a long while in memory, and the reset waits for
# the fold's locks meanwhile.  The worker's fold is held just before the
# sort, interrupts held, while the reset asks it to cancel; once let go,
# it takes the cancel in the sort, quietly, and never reaches the point
# after it (which raises an error if it does).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('fold_cancel_in_sort');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = on
prolly_fold_worker_naptime = 50
fsync = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{ALTER DATABASE postgres SET prolly.root_interval = 0;
	ALTER DATABASE postgres SET prolly.root_interval_commits = 1});
sql(q{CREATE EXTENSION injection_points;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, rev int NOT NULL, payload text NOT NULL);
	INSERT INTO app.t SELECT g, 0, repeat(md5(g::text), 4)
	  FROM generate_series(1, 20000) g;
	SELECT dolt_commit('-Am', 'load') IS NOT NULL});
my $target = sql(q{UPDATE app.t SET rev = 1 WHERE id % 100 = 0;
	SELECT (dolt_commit('-am', 'r1'))[1]});
my $digest = q{SELECT md5(string_agg(id || ':' || rev, ',' ORDER BY id)) FROM app.t};
my $at_target = sql($digest);

my $fn = sql(q{SELECT pg_relation_filenode('app.t')});
sql(qq{SELECT injection_points_attach('prolly-fold-sort', 'wait', '$fn');
	SELECT injection_points_attach('prolly-fold-sorted', 'error', '$fn')});
sql(q{UPDATE app.t SET rev = 99 WHERE id <= 18000});
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE backend_type = 'prolly fold worker'
		  AND wait_event = 'prolly-fold-sort'}),
	'the fold worker is stopped before its sort, holding app.t');

my $cancels = sql(q{SELECT value FROM pg_stat_prolly WHERE name = 'restores_fold_cancels'}) || 0;

# (the reset waits for the worker it asked as long as it takes here)
my $reset = $node->background_psql('postgres');
$reset->query_until(qr/started/, qq{
	SET deadlock_timeout = '5min';
	\\echo started
	SELECT 'reset ' || (dolt_reset('--hard', '$target') IS NOT NULL);
});
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'BranchKeptLock'}),
	'the reset asked the fold to cancel, and waits for its lock');

sql(q{SELECT injection_points_detach('prolly-fold-sort');
	SELECT injection_points_wakeup('prolly-fold-sort')});
my $out = $reset->query_safe(q{SELECT pg_stat_force_next_flush(); SELECT 'done'});
$reset->quit;
sql(q{SELECT injection_points_detach('prolly-fold-sorted')});

ok( sql(q{SELECT value FROM pg_stat_prolly WHERE name = 'restores_fold_cancels'}) > $cancels,
	'the fold was asked to cancel');
is(sql($digest), $at_target, 'the reset gave the commit\'s rows');
unlike(slurp_file($node->logfile), qr/error triggered for injection point prolly-fold-sorted/,
	'the fold took the cancel in its sort, never finishing it');
unlike(slurp_file($node->logfile), qr/canceling statement/,
	'the cancelled fold reported no error');

$node->stop;
done_testing();
