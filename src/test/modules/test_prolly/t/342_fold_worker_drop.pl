# Copyright (c) 2026, PostgreSQL Global Development Group

# The fold worker never waits for a lock.  Its fold keeps the locks on the
# change logs it consumed past its lock on their table, so a DROP of
# several tables, holding the tables and then waiting for one's log, would
# deadlock with the fold waiting for the next table.  Here the worker
# stops once it has folded a, then b, holding a's log but not a; a DROP of
# a and c then waits for a's log; the worker, going on to c, rolls its fold
# back instead of waiting, and the DROP goes through.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('fold_worker_drop');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = on
prolly_fold_worker_naptime = 200ms
prolly_fold_worker_batch_rows = 0
deadlock_timeout = 100ms
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION injection_points;
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.a (id int PRIMARY KEY, v int);
	CREATE TABLE s.b (id int PRIMARY KEY, v int);
	CREATE TABLE s.c (id int PRIMARY KEY, v int);
	INSERT INTO s.a VALUES (1, 0);
	INSERT INTO s.b VALUES (1, 0);
	INSERT INTO s.c VALUES (1, 0);
	SELECT dolt_commit('-Am', 'base');
});
my $empty = sql(q{SELECT 'SELECT ' || string_agg('(SELECT count(*) FROM pg_toast.pg_prolly_log_' || oid || ')', ' + ') || ' = 0'
	FROM pg_class WHERE oid IN ('s.a'::regclass, 's.b'::regclass, 's.c'::regclass)});
ok($node->poll_query_until('postgres', $empty), 'the worker folded the base');

my $stopped = q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE backend_type = 'prolly fold worker' AND wait_event = 'prolly-log-trimmed'};
my $worker_holds = q{SELECT array_agg(l.relation::regclass::text ORDER BY 1)
	FROM pg_locks l JOIN pg_stat_activity w USING (pid)
	WHERE w.backend_type = 'prolly fold worker' AND l.locktype = 'relation'
	  AND l.relation IN ('s.a'::regclass, 's.b'::regclass, 's.c'::regclass)};

# the worker folds a, b and c in that order
sql(q{SELECT injection_points_attach('prolly-log-trimmed', 'wait')});
sql(q{UPDATE s.a SET v = 1; UPDATE s.b SET v = 1; UPDATE s.c SET v = 1});
ok($node->poll_query_until('postgres', $stopped), 'the worker folded a');
sql(q{SELECT injection_points_wakeup('prolly-log-trimmed')});
$node->poll_query_until('postgres', qq{SELECT ($worker_holds) = '{s.b}'})
  or die 'the worker did not go on to b';
ok($node->poll_query_until('postgres', $stopped), 'the worker folded b');
sql(q{SELECT injection_points_detach('prolly-log-trimmed')});
is(sql($worker_holds), '{s.b}', 'holding b but not a');

# the DROP holds a and c, and waits for a's log, which the worker holds
my $log_offset = -s $node->logfile;
my $drop = $node->background_psql('postgres', on_error_stop => 0);
$drop->query_until(qr/started/, q{
	\echo started
	DROP TABLE s.a, s.c;
	\echo done
});
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		  WHERE query LIKE 'DROP TABLE s.a%' AND wait_event_type = 'Lock'}),
	'the drop waits for the worker');

# the worker goes on to c: it rolls its fold back rather than wait
sql(q{SELECT injection_points_wakeup('prolly-log-trimmed')});
$drop->query_until(qr/done/, '');
unlike($drop->{stderr}, qr/ERROR/, 'the drop went through');
is(sql(q{SELECT count(*) FROM pg_class WHERE relname IN ('a', 'c') AND relnamespace = 's'::regnamespace}),
	'0', 'a and c are gone');
ok(!$node->log_contains('deadlock detected', $log_offset), 'no deadlock');

# and the worker folds the branch next pass
my $b_empty = sql(q{SELECT 'SELECT count(*) = 0 FROM pg_toast.pg_prolly_log_' || 's.b'::regclass::oid});
ok($node->poll_query_until('postgres', $b_empty), 'the worker folds b again');

$drop->quit;
$node->stop;
done_testing();
