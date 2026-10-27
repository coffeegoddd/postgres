# Copyright (c) 2026, PostgreSQL Global Development Group

# A hard reset gives a table back its kept pages under the table's locks,
# queued for as TRUNCATE queues them: a holder holds the reset up rather
# than turning the restore away, as giving up would leave the reset every
# row the working set changed to write back, however many:
#
# - an autovacuum worker vacuuming the branch's copy is cancelled after
#   deadlock_timeout, as Postgres cancels one blocking a lock waiter, and
#   the reset restores;
# - another holder is waited for, however long it holds the lock: the
#   reset restores once it lets go, writing no row back;
# - lock_timeout bounds the wait: the reset then fails, the working set
#   left as it was, and still writes no row.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time);

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('restore_lock_autovacuum');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = on
autovacuum_naptime = 1s
prolly_fold_worker = off
log_autovacuum_min_duration = 0
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# (the last line: a checkout's own output comes first)
sub on_scratch
{
	my @out = split /\n/, sql(qq{SELECT dolt_checkout('scratch'); $_[0]});
	return $out[-1];
}

sql(q{
	CREATE EXTENSION injection_points;
	ALTER DATABASE postgres SET prolly.restore_min_changes = 0;
	CREATE SCHEMA kp VERSIONED;
	CREATE TABLE kp.t (a int PRIMARY KEY, b text)
		WITH (autovacuum_vacuum_threshold = 0,
			  autovacuum_vacuum_scale_factor = 0);
	INSERT INTO kp.t SELECT g, 'r' || g FROM generate_series(1, 4000) g;
	CREATE TABLE kp.u (a int PRIMARY KEY, b text)
		WITH (autovacuum_vacuum_threshold = 0,
			  autovacuum_vacuum_scale_factor = 0,
			  autovacuum_vacuum_cost_delay = 100,
			  autovacuum_vacuum_cost_limit = 1,
			  autovacuum_vacuum_insert_threshold = -1,
			  autovacuum_analyze_threshold = 1000000000);
	INSERT INTO kp.u SELECT g, 'r' || g FROM generate_series(1, 4000) g;
	SELECT dolt_commit('-Am', 'load');
	SELECT dolt_branch('scratch');
});
my $at_head = on_scratch(q{SELECT md5(string_agg(t::text, ',' ORDER BY a)) FROM kp.t});

sub counted
{
	my ($name) = @_;
	return sql(qq{SELECT coalesce((SELECT value FROM pg_stat_prolly
		WHERE name = '$name'), 0)});
}

# a 'wait' point detached, and any process stopped there woken
sub release
{
	my ($point) = @_;
	sql(qq{SELECT injection_points_detach('$point')});
	# (none waits if none got there)
	$node->psql('postgres', qq{SELECT injection_points_wakeup('$point')});
}

# 5% of the scratch branch's rows changed
sub dirty
{
	on_scratch(q{UPDATE kp.t SET b = b || 'x' WHERE a % 20 = 0});
}

# a hard reset of the scratch branch in a session of its own, started:
# its settings first
sub start_reset
{
	my ($settings) = @_;
	my $reset = $node->background_psql('postgres');
	$reset->query_safe(qq{SELECT dolt_checkout('scratch'); $settings});
	my $pid = $reset->query_safe(q{SELECT pg_backend_pid()});
	$reset->query_until(qr/started/, q{
		\echo started
		SELECT 'reset ' || (dolt_reset('--hard') IS NOT NULL);
		SELECT pg_stat_force_next_flush();
		\echo done
	});
	return ($reset, $pid);
}

sub finish_reset
{
	my ($reset) = @_;
	my $out = $reset->query_until(qr/done/, '');
	$reset->quit;
	return $out;
}

sub at_head
{
	return on_scratch(q{SELECT md5(string_agg(t::text, ',' ORDER BY a)) FROM kp.t})
	  eq $at_head;
}

# 1. An autovacuum worker vacuuming the scratch branch's copy of kp.t,
# stopped holding it.  The point is told each relation's file number, and
# stops at the scratch copy's only (main's has its own).
my $filenode = on_scratch(q{SELECT pg_relation_filenode('kp.t')});
sql(qq{SELECT injection_points_attach('vacuum-relation-held', 'wait', '$filenode')});
dirty();
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE backend_type = 'autovacuum worker'
		  AND wait_event = 'vacuum-relation-held'}),
	'an autovacuum worker holds the scratch copy');
my $worker = sql(q{SELECT pid FROM pg_stat_activity
	WHERE backend_type = 'autovacuum worker'
	  AND wait_event = 'vacuum-relation-held'});
ok( sql(qq{SELECT count(*) > 0 FROM pg_locks WHERE pid = $worker
		AND relation = 'kp.t'::regclass AND granted}),
	'the worker holds kp.t');

my $restored = counted('relations_restored');
my $applied = counted('apply_rows');
my ($reset) = start_reset(q{SET deadlock_timeout = '100ms'});
like(finish_reset($reset), qr/reset t/, 'the reset with autovacuum holding finished');
# (no worker launched since is to stop at the point)
release('vacuum-relation-held');
ok(counted('relations_restored') > $restored,
	'the reset restored the table the autovacuum held');
is(counted('apply_rows'), $applied, 'and wrote no row back');
ok( $node->poll_query_until(
		'postgres', qq{SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $worker
		AND wait_event = 'vacuum-relation-held'}),
	'the autovacuum was cancelled');
my $cancels = () = slurp_file($node->logfile) =~
  /autovacuum worker\[$worker\] ERROR:  canceling autovacuum task/g;
is($cancels, 1, 'the worker was cancelled once');
ok(at_head(), 'the scratch branch holds its head\'s rows');

# 2. Another session on the scratch branch holding kp.t, as long as it
# likes: the reset waits in the lock's queue, and restores once it lets go.
dirty();
my $holder = $node->background_psql('postgres');
$holder->query_safe(q{SELECT dolt_checkout('scratch'); BEGIN;
	LOCK TABLE kp.t IN SHARE UPDATE EXCLUSIVE MODE});
$restored = counted('relations_restored');
$applied = counted('apply_rows');
my $busy = counted('restores_lock_busy');
my $resetter;
($reset, $resetter) = start_reset(q{SET prolly.restore_lock_wait = 0});
ok( $node->poll_query_until(
		'postgres', qq{SELECT wait_event_type = 'Lock'
		FROM pg_stat_activity WHERE pid = $resetter}),
	'the reset waits in the queue of the holder\'s lock');
# (well past prolly.restore_lock_wait, which bounds only a share's tries)
sleep(1);
ok( sql(qq{SELECT wait_event_type = 'Lock' FROM pg_stat_activity
		WHERE pid = $resetter}),
	'and goes on waiting');
$holder->query_safe(q{COMMIT});
like(finish_reset($reset), qr/reset t/, 'the reset finished once the holder let go');
ok(counted('relations_restored') > $restored,
	'the reset restored the table once the holder let go');
is(counted('apply_rows'), $applied, 'writing no row back');
is(counted('restores_lock_busy'), $busy, 'no restore was given up on');
ok(at_head(), 'the scratch branch holds its head\'s rows again');

# 3. lock_timeout bounds the wait: the reset fails, and its working set is
# as it was; once the holder lets go, the reset restores.
dirty();
my $dirty = on_scratch(q{SELECT md5(string_agg(t::text, ',' ORDER BY a)) FROM kp.t});
$holder->query_safe(q{BEGIN; LOCK TABLE kp.t IN SHARE UPDATE EXCLUSIVE MODE});
$applied = counted('apply_rows');
my $began = time();
my ($ret, $out, $err) = $node->psql('postgres',
	q{SELECT dolt_checkout('scratch'); SET lock_timeout = '300ms';
	  SELECT dolt_reset('--hard')});
my $took = time() - $began;
like($err, qr/canceling statement due to lock timeout/,
	'with lock_timeout, the reset fails once it passes');
ok($took >= 0.3 && $took < 10, "after lock_timeout ($took s)");
is(counted('apply_rows'), $applied, 'having written no row');
is(on_scratch(q{SELECT md5(string_agg(t::text, ',' ORDER BY a)) FROM kp.t}),
	$dirty, 'the working set is as it was');
$holder->query_safe(q{COMMIT});
$holder->quit;
$restored = counted('relations_restored');
on_scratch(q{SELECT dolt_reset('--hard'); SELECT pg_stat_force_next_flush()});
ok(counted('relations_restored') > $restored, 'the reset then restores');
ok(at_head(), 'the scratch branch holds its head\'s rows at last');

$node->stop;
done_testing();
