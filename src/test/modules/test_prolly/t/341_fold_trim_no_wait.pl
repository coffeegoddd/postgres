# Copyright (c) 2026, PostgreSQL Global Development Group

# Folds of the same branch trim the same rows of its tables' change logs:
# a commit's fold and the fold worker's, which takes no lock the commit
# waits for.  Each trims its tables in its own order, so one waiting for
# the other's deletes in one log while the other waits for its deletes in
# another deadlocks.  A trim never waits: a row another transaction is
# deleting is left to it.  Here the worker stops once it has trimmed a
# log, its deletes not committed, and a commit folding the branch then
# goes through without waiting for it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('fold_trim_no_wait');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = on
prolly_fold_worker_naptime = 200ms
prolly_fold_worker_batch_rows = 0
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION injection_points;
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.a (id int PRIMARY KEY, v int);
	CREATE TABLE s.b (id int PRIMARY KEY, v int);
	INSERT INTO s.a SELECT g, 0 FROM generate_series(1, 100) g;
	INSERT INTO s.b SELECT g, 0 FROM generate_series(1, 100) g;
	SELECT dolt_commit('-Am', 'base');
});
my $empty = sql(q{SELECT 'SELECT ' || string_agg('(SELECT count(*) FROM pg_toast.pg_prolly_log_' || oid || ')', ' + ') || ' = 0'
	FROM pg_class WHERE oid IN ('s.a'::regclass, 's.b'::regclass)});
ok($node->poll_query_until('postgres', $empty), 'the worker folded the base');

# the worker stops once it has trimmed a log, its deletes uncommitted
sql(q{SELECT injection_points_attach('prolly-log-trimmed', 'wait')});
sql(q{UPDATE s.a SET v = 1; UPDATE s.b SET v = 1});
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		  WHERE backend_type = 'prolly fold worker'
		    AND wait_event = 'prolly-log-trimmed'}),
	'the worker trimmed a log');
sql(q{SELECT injection_points_detach('prolly-log-trimmed')});

# a commit's fold trims the same rows: it goes through without waiting
my $log_offset = -s $node->logfile;
my ($ret, $out, $err) = $node->psql('postgres', q{
	SET lock_timeout = '10s';
	SELECT count(*) FROM dolt_commit('-Am', 'c2');
});
is($ret, 0, 'the commit did not wait for the worker\'s trim');
is($err, '', 'with no error');

# the worker goes on: no deadlock, and the logs end up trimmed
sql(q{SELECT injection_points_wakeup('prolly-log-trimmed')});
ok($node->poll_query_until('postgres', $empty), 'the logs are trimmed');
ok(!$node->log_contains('deadlock detected', $log_offset), 'no deadlock');
is(sql(q{SELECT count(*) FROM s.a WHERE v = 1}) . ' ' . sql(q{SELECT count(*) FROM s.b WHERE v = 1}),
	'100 100', 'the rows are as written');
is(sql(q{SELECT message FROM dolt_log() LIMIT 1}), 'c2', 'the commit is the head');
is(sql(q{SELECT count(*) FROM dolt_status()}), '0', 'and the working set is clean');

$node->stop;
done_testing();
