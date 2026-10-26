# Copyright (c) 2026, PostgreSQL Global Development Group

# A hard reset gives a table back its kept pages under the table's locks,
# which it only tries: it isn't in the locks' queues, so as not to hold up
# the writers behind it.  A holder of one doesn't turn the restore away at
# once (the reset then writing the dirty rows back instead):
#
# - an autovacuum worker vacuuming the branch's copy is asked to cancel, as
#   Postgres cancels one that blocks a lock waiter, and waited for however
#   short prolly.restore_lock_wait is (even 0): the reset restores;
# - another holder is waited for, for prolly.restore_lock_wait: the reset
#   restores once it lets go;
# - one still there after that is given up on, the restore counted in
#   restores_lock_busy, and the reset writes the rows; lock_timeout caps
#   the wait, and doesn't lengthen it.

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
my $busy = counted('restores_lock_busy');
my ($reset) = start_reset(q{SET prolly.restore_lock_wait = 0});
like(finish_reset($reset), qr/reset t/, 'the reset with autovacuum holding finished');
# (no worker launched since is to stop at the point)
release('vacuum-relation-held');
ok(counted('relations_restored') > $restored,
	'the reset restored the table the autovacuum held');
is(counted('restores_lock_busy'), $busy, 'no restore was given up on');
ok( $node->poll_query_until(
		'postgres', qq{SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $worker
		AND wait_event = 'vacuum-relation-held'}),
	'the autovacuum was cancelled');
ok(at_head(), 'the scratch branch holds its head\'s rows');

# 1b. A worker asked to cancel is asked once.  Stopped in its error
# handling, the lock still held, it is the same transaction holding it to
# the reset's retries: asked again there, it would take the second cancel
# once its handling is done, outside it, and exit, leaving the rest of its
# tables.  The reset gives up on it (deadlock_timeout), writing the rows.
$filenode = on_scratch(q{SELECT pg_relation_filenode('kp.t')});
sql(qq{SELECT injection_points_attach('vacuum-relation-held', 'wait', '$filenode');
	SELECT injection_points_attach('autovacuum-table-error', 'wait')});
dirty();
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE backend_type = 'autovacuum worker'
		  AND wait_event = 'vacuum-relation-held'}),
	'an autovacuum worker holds the scratch copy again');
$worker = sql(q{SELECT pid FROM pg_stat_activity
	WHERE backend_type = 'autovacuum worker'
	  AND wait_event = 'vacuum-relation-held'});
$busy = counted('restores_lock_busy');
my $resetter;
($reset, $resetter) = start_reset(q{SET prolly.restore_lock_wait = 0});
ok( $node->poll_query_until(
		'postgres', qq{SELECT wait_event = 'autovacuum-table-error'
		FROM pg_stat_activity WHERE pid = $worker}),
	'the worker asked to cancel stopped in its error handling');
like(finish_reset($reset), qr/reset t/,
	'the reset with the worker in its error handling finished');
ok(counted('restores_lock_busy') > $busy,
	'the reset gave up on the worker still holding the lock');
release('vacuum-relation-held');
release('autovacuum-table-error');
ok( $node->poll_query_until(
		'postgres', qq{SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $worker}),
	'the worker went on');
my $cancels = () = slurp_file($node->logfile) =~
  /autovacuum worker\[$worker\] ERROR:  canceling autovacuum task/g;
is($cancels, 1, 'the worker was cancelled once');
ok(at_head(), 'the scratch branch holds its head\'s rows after it');

# 2. Another session on the scratch branch holding kp.t, letting go while
# the reset waits for it (prolly.restore_lock_wait well past that)
dirty();
my $holder = $node->background_psql('postgres');
$holder->query_safe(q{SELECT dolt_checkout('scratch'); BEGIN;
	LOCK TABLE kp.t IN SHARE UPDATE EXCLUSIVE MODE});
$restored = counted('relations_restored');
($reset, $resetter) = start_reset(q{SET prolly.restore_lock_wait = '3min'});
ok( $node->poll_query_until(
		'postgres', qq{SELECT wait_event = 'BranchKeptLock'
		FROM pg_stat_activity WHERE pid = $resetter}),
	'the reset waits for the lock holder');
$holder->query_safe(q{COMMIT});
like(finish_reset($reset), qr/reset t/, 'the reset finished once the holder let go');
ok(counted('relations_restored') > $restored,
	'the reset restored the table once the holder let go');
ok(at_head(), 'the scratch branch holds its head\'s rows again');

# 3. A holder that doesn't let go: given up on after the wait, the restore
# counted, and the rows written instead.  The wait is the smaller of
# prolly.restore_lock_wait and lock_timeout.
sub given_up
{
	my ($settings, $at_least, $under, $what) = @_;
	dirty();
	$holder->query_safe(q{BEGIN; LOCK TABLE kp.t IN SHARE UPDATE EXCLUSIVE MODE});
	my $restored = counted('relations_restored');
	my $busy = counted('restores_lock_busy');
	my $applied = counted('apply_rows');
	my $began = time();
	my ($reset) = start_reset($settings);
	like(finish_reset($reset), qr/reset t/, "$what: the reset finished");
	my $took = time() - $began;
	ok($took >= $at_least && $took < $under,
		"$what: the reset waited $at_least s and less than $under s ($took s)");
	is(counted('relations_restored'), $restored, "$what: nothing restored");
	ok(counted('restores_lock_busy') > $busy, "$what: the restore given up on is counted");
	ok(counted('apply_rows') > $applied, "$what: the reset wrote the rows instead");
	$holder->query_safe(q{COMMIT});
	ok(at_head(), "$what: the scratch branch holds its head's rows");
}

given_up(q{SET prolly.restore_lock_wait = '300ms'}, 0.3, 10,
	'prolly.restore_lock_wait alone');
given_up(q{SET prolly.restore_lock_wait = '20s'; SET lock_timeout = '300ms'}, 0.3, 10,
	'lock_timeout below prolly.restore_lock_wait');
given_up(q{SET prolly.restore_lock_wait = '300ms'; SET lock_timeout = '20s'}, 0.3, 10,
	'lock_timeout above prolly.restore_lock_wait');
$holder->quit;

$node->stop;
done_testing();
