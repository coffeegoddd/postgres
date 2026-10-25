# Copyright (c) 2026, PostgreSQL Global Development Group

# The fold worker's fold trims the change-log rows it folds of a writer
# that committed while the fold was starting: its own older snapshot of
# the pass doesn't hold the trim back.  The fold reads by a snapshot
# taken as it begins, which sees the writer committed; had the worker
# kept the snapshot it started the pass by (from while the writer was
# running), the trim would keep the writer's rows, and the fold after,
# finding nothing new to fold, would leave them in the log for good.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('fold_worker_trim_own_snapshot');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = on
prolly_fold_worker_naptime = 50ms
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION injection_points;
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	INSERT INTO s.t SELECT g, 'x' FROM generate_series(1, 1000) g;
	SELECT dolt_commit('-Am', 'base');
});
my $log = sql(q{SELECT 'pg_toast.pg_prolly_log_' || 's.t'::regclass::oid});
my $empty = qq{SELECT count(*) = 0 FROM $log};
ok($node->poll_query_until('postgres', $empty), 'the log is empty to begin with');

# A writer running as the worker begins a fold: the fold, held at its
# start, began its pass while the writer ran.
my $writer = $node->background_psql('postgres');
$writer->query_safe(q{BEGIN; UPDATE s.t SET v = 'w' WHERE id <= 100});
sql(q{SELECT injection_points_attach('prolly-fold-worker-fold', 'wait')});
sql(q{UPDATE s.t SET v = 'moved' WHERE id = 1000});
ok( $node->poll_query_until(
		'postgres',
		q{SELECT count(*) = 1 FROM pg_stat_activity
		  WHERE backend_type = 'prolly fold worker'
		    AND wait_event = 'prolly-fold-worker-fold'}),
	'a fold begins while the writer runs');

# The writer commits; then the fold goes on, and sees it committed.
$writer->query_safe('COMMIT');
$writer->quit;
sql(q{SELECT injection_points_detach('prolly-fold-worker-fold')});
sql(q{SELECT injection_points_wakeup('prolly-fold-worker-fold')});

# Its fold, or the next, trims the writer's rows, and the other's: the
# log empties though nothing writes the table again.
ok($node->poll_query_until('postgres', $empty),
	'the log empties after the writer that committed as the fold began');
is(sql(q{SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::s.t)}),
	'101', 'both writes are in the working set');

$node->stop;
done_testing();
