# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum's automatic fold, too, never waits for a relation's lock.
# Like the fold worker's, its fold keeps the locks on the change logs it
# consumed past its lock on their table: stopped once it has folded a,
# then b, it holds a's log but not a; a DROP of a and c waits for a's log;
# the fold, going on to c, is given up rather than wait, and the DROP goes
# through.  The tables are folded again later.  (deadlock_timeout is long:
# a DROP waiting that long for an autovacuum worker cancels it, which
# would hide the cycle.)

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('autofold_drop');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = on
autovacuum_naptime = 1s
prolly_fold_worker = off
prolly.autofold_threshold = 0.001
deadlock_timeout = 5s
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION injection_points;
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.a (id int PRIMARY KEY, v int);
	CREATE TABLE s.b (id int PRIMARY KEY, v int);
	CREATE TABLE s.c (id int PRIMARY KEY, v int);
	INSERT INTO s.a SELECT g, 0 FROM generate_series(1, 2000) g;
	INSERT INTO s.b SELECT g, 0 FROM generate_series(1, 2000) g;
	INSERT INTO s.c SELECT g, 0 FROM generate_series(1, 2000) g;
	SELECT dolt_commit('-Am', 'base');
});

my $stopped = q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE backend_type = 'autovacuum worker' AND wait_event = 'prolly-log-trimmed'};
my $folder_holds = q{SELECT array_agg(l.relation::regclass::text ORDER BY 1)
	FROM pg_locks l JOIN pg_stat_activity w USING (pid)
	WHERE w.backend_type = 'autovacuum worker' AND l.locktype = 'relation'
	  AND l.relation IN ('s.a'::regclass, 's.b'::regclass, 's.c'::regclass)};
my $b_empty = sql(q{SELECT 'SELECT count(*) = 0 FROM pg_toast.pg_prolly_log_' || 's.b'::regclass::oid});

# the automatic fold folds a, b and c in that order
sql(q{SELECT injection_points_attach('prolly-log-trimmed', 'wait')});
sql(q{UPDATE s.a SET v = 1; UPDATE s.b SET v = 1; UPDATE s.c SET v = 1});
ok($node->poll_query_until('postgres', $stopped), 'autovacuum folded a');
sql(q{SELECT injection_points_wakeup('prolly-log-trimmed')});
$node->poll_query_until('postgres', qq{SELECT ($folder_holds) = '{s.b}'})
  or die 'the fold did not go on to b';
ok($node->poll_query_until('postgres', $stopped), 'autovacuum folded b');
sql(q{SELECT injection_points_detach('prolly-log-trimmed')});
is(sql($folder_holds), '{s.b}', 'holding b but not a');

# the DROP holds a and c, and waits for a's log, which the fold holds
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
	'the drop waits for the automatic fold');

# the fold goes on to c: given up rather than wait
sql(q{SELECT injection_points_wakeup('prolly-log-trimmed')});
$drop->query_until(qr/done/, '');
unlike($drop->{stderr}, qr/ERROR/, 'the drop went through');
is(sql(q{SELECT count(*) FROM pg_class WHERE relname IN ('a', 'c') AND relnamespace = 's'::regnamespace}),
	'0', 'a and c are gone');
ok(!$node->log_contains('deadlock detected', $log_offset), 'no deadlock');
ok(!$node->log_contains('could not obtain lock', $log_offset), 'nor an error');
ok($node->poll_query_until('postgres', $b_empty), 'b is folded again');

$drop->quit;
$node->stop;
done_testing();
