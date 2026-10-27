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
#   left as it was, and still writes no row;
# - the reset queues before it holds the refs, so a checkout of another
#   branch doesn't wait behind it.
#
# A reset in a transaction that holds the refs already (a branch made
# first) can't queue for its locks without holding up every other
# branch's operation, so it tries them, as a share does, within bounds:
#
# - an autovacuum worker is asked to cancel once, and waited for until
#   deadlock_timeout;
# - once the workers' wait is spent, a worker found on a later table isn't
#   asked;
# - another holder is waited for until prolly.restore_lock_wait (or
#   lock_timeout, if smaller), then given up on, counted in
#   restores_lock_busy, and the rows written.

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
# (in_refs: in a transaction holding the refs already, a branch made first)
my $held = 0;
sub start_reset
{
	my ($settings, $in_refs) = @_;
	my $reset = $node->background_psql('postgres');
	$reset->query_safe(qq{SELECT dolt_checkout('scratch'); $settings});
	my $pid = $reset->query_safe(q{SELECT pg_backend_pid()});
	my $begin = '';
	my $end = '';
	if ($in_refs)
	{
		$held++;
		$begin = "BEGIN; SELECT dolt_branch('held_$held') IS NOT NULL;";
		$end = 'COMMIT;';
	}
	$reset->query_until(qr/started/, qq{
		\\echo started
		$begin
		SELECT 'reset ' || (dolt_reset('--hard') IS NOT NULL);
		$end
		SELECT pg_stat_force_next_flush();
		\\echo done
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

# 4. A reader of main's copy left in an open transaction, and a reset of
# main waiting for it: a checkout of another branch, which waits for the
# refs, doesn't wait behind the reset, which doesn't hold them yet.
sql(q{UPDATE kp.t SET b = b || 'm' WHERE a % 20 = 0});
my $reader = $node->background_psql('postgres');
$reader->query_safe(q{BEGIN; SELECT count(*) FROM kp.t});
$restored = counted('relations_restored');
my $mreset = $node->background_psql('postgres');
my $mresetter = $mreset->query_safe(q{SELECT pg_backend_pid()});
$mreset->query_until(qr/started/, q{
	\echo started
	SELECT 'reset ' || (dolt_reset('--hard') IS NOT NULL);
	SELECT pg_stat_force_next_flush();
	\echo done
});
ok( $node->poll_query_until(
		'postgres', qq{SELECT wait_event_type = 'Lock' AND wait_event = 'relation'
		FROM pg_stat_activity WHERE pid = $mresetter}),
	'a reset of main waits for the reader');
$began = time();
is(on_scratch(q{SELECT count(*) FROM kp.t}), '4000',
	'a checkout of another branch meanwhile');
$took = time() - $began;
ok($took < 5, "doesn't wait behind the reset ($took s)");
is(sql(q{SELECT dolt_branch('meanwhile') IS NOT NULL}), 't',
	'nor does making a branch');
$reader->query_safe(q{COMMIT});
$reader->quit;
like($mreset->query_until(qr/done/, ''), qr/reset t/,
	'the reset finished once the reader ended');
$mreset->quit;
ok(counted('relations_restored') > $restored, 'restoring main\'s table');

# 5. In a transaction holding the refs already: a worker asked to cancel
# is asked once.  Stopped in its error handling, the lock still held, it
# is the same transaction holding it to the reset's retries: asked again
# there, it would take the second cancel once its handling is done,
# outside it, and exit, leaving the rest of its tables.  The reset gives
# up on it (deadlock_timeout), writing the rows.
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
$applied = counted('apply_rows');
($reset, $resetter) = start_reset(q{SET prolly.restore_lock_wait = 0}, 1);
ok( $node->poll_query_until(
		'postgres', qq{SELECT wait_event = 'autovacuum-table-error'
		FROM pg_stat_activity WHERE pid = $worker}),
	'the worker asked to cancel stopped in its error handling');
like(finish_reset($reset), qr/reset t/,
	'the reset with the worker in its error handling finished');
ok(counted('restores_lock_busy') > $busy,
	'the reset gave up on the worker still holding the lock');
ok(counted('apply_rows') > $applied, 'and wrote the rows');
release('vacuum-relation-held');
release('autovacuum-table-error');
ok( $node->poll_query_until(
		'postgres', qq{SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $worker}),
	'the worker went on');
$cancels = () = slurp_file($node->logfile) =~
  /autovacuum worker\[$worker\] ERROR:  canceling autovacuum task/g;
is($cancels, 1, 'the worker was cancelled once');
ok(at_head(), 'the scratch branch holds its head\'s rows after it');

# 6. Once a move's waits for workers are spent, a worker it finds on a
# later table isn't asked to cancel: the move writes that table's rows
# anyway, and the cancel would only cost the worker its vacuum.  kp.u's
# vacuum crawls (its cost options), holding it; kp.t's worker is stopped
# holding it.  Whichever table the reset tries first, its worker is asked
# and stops in its error handling, still holding the table, until the
# workers' wait (deadlock_timeout) is spent; the other table's worker is
# then left alone.
my $u_at_head = on_scratch(q{SELECT md5(string_agg(u::text, ',' ORDER BY a)) FROM kp.u});
on_scratch(q{UPDATE kp.u SET b = b || 'x' WHERE a % 20 = 0});
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) > 0 FROM pg_stat_progress_vacuum
		WHERE relid = 'kp.u'::regclass}),
	'an autovacuum worker vacuums the scratch copy of kp.u');
$filenode = on_scratch(q{SELECT pg_relation_filenode('kp.t')});
sql(qq{SELECT injection_points_attach('vacuum-relation-held', 'wait', '$filenode');
	SELECT injection_points_attach('autovacuum-table-error', 'wait')});
dirty();
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE backend_type = 'autovacuum worker'
		  AND wait_event = 'vacuum-relation-held'}),
	'another autovacuum worker holds the scratch copy of kp.t');
($reset) = start_reset(q{SET prolly.restore_lock_wait = 0; SET deadlock_timeout = '200ms'}, 1);
like(finish_reset($reset), qr/reset t/, 'the reset with two workers holding finished');
is( sql(q{SELECT count(*) FROM pg_stat_activity
		WHERE backend_type = 'autovacuum worker'
		  AND wait_event = 'autovacuum-table-error'}),
	'1', 'only the first worker found was asked to cancel');
release('vacuum-relation-held');
release('autovacuum-table-error');
ok(at_head(), 'the scratch branch holds its head\'s rows of kp.t');
is(on_scratch(q{SELECT md5(string_agg(u::text, ',' ORDER BY a)) FROM kp.u}),
	$u_at_head, 'and of kp.u');

# 7. A holder that doesn't let go, in a transaction holding the refs
# already: given up on after the wait, the restore counted, and the rows
# written instead.  The wait is the smaller of prolly.restore_lock_wait
# and lock_timeout.
$holder = $node->background_psql('postgres');
$holder->query_safe(q{SELECT dolt_checkout('scratch')});
# (kp.u, its rows written back by the last reset, given its kept pages
# first, so that the resets below restore nothing of it)
on_scratch(q{SELECT dolt_reset('--hard')});
sub given_up
{
	my ($settings, $at_least, $under, $what) = @_;
	dirty();
	$holder->query_safe(q{BEGIN; LOCK TABLE kp.t IN SHARE UPDATE EXCLUSIVE MODE});
	my $restored = counted('relations_restored');
	my $busy = counted('restores_lock_busy');
	my $applied = counted('apply_rows');
	my $began = time();
	my ($reset) = start_reset($settings, 1);
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
