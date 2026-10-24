# Copyright (c) 2026, PostgreSQL Global Development Group

# prolly_fold_worker_cost_delay: the fold worker's folds paced by the
# cost-based delay, as vacuum is.  Off (the default), a fold naps never;
# on, a fold that has spent prolly_fold_worker_cost_limit naps
# (fold_worker_cost_delays counts the naps), and folds the same rows.  Off
# again, it naps no more: nothing of the pacing outlives the folds paced.
# A paced fold gives way to a lock request on what it holds (a retry), naps
# for 10 s in all at most, and takes a reload of the settings under way.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('fold_worker_pacing');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = on
prolly_fold_worker_naptime = 50ms
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub counter
{
	my ($name) = @_;
	return sql(qq{SELECT value FROM pg_stat_prolly WHERE name = '$name'});
}

sub set_conf
{
	my ($name, $value) = @_;
	sql(qq{ALTER SYSTEM SET $name = '$value'});
	sql(q{SELECT pg_reload_conf()});
}

sql(q{
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	INSERT INTO s.t SELECT g, 'x' FROM generate_series(1, 2000) g;
	SELECT dolt_commit('-Am', 'base');
});
my $log = sql(q{SELECT 'pg_toast.pg_prolly_log_' || 's.t'::regclass::oid});
my $folded = qq{SELECT count(*) = 0 FROM $log};

# Writes, and the worker's fold of them, waited for: its counts flushed by
# then (the worker reports them after each pass).
sub fold_writes
{
	my ($v, $what) = @_;
	my $folds = counter('fold_worker_folds');

	sql(qq{UPDATE s.t SET v = '$v' WHERE id <= 500});
	ok($node->poll_query_until('postgres', $folded), "$what: folded");
	ok( $node->poll_query_until(
			'postgres',
			qq{SELECT value > $folds FROM pg_stat_prolly
			   WHERE name = 'fold_worker_folds'}),
		"$what: counted");
	is(sql(qq{SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::s.t)
			  WHERE (to_row).v = '$v'}),
		'500', "$what: the rows folded");
}

# Off: no fold naps.
fold_writes('off', 'unpaced');
is(counter('fold_worker_cost_delays'), '0', 'an unpaced fold never naps');

# On, with a limit a fold spends at once: its folds nap.
set_conf('prolly_fold_worker_cost_limit', '1');
set_conf('prolly_fold_worker_cost_delay', '1ms');
fold_writes('on', 'paced');
my $naps = counter('fold_worker_cost_delays');
cmp_ok($naps, '>', 0, 'a paced fold naps');
is(sql(q{SELECT count(*) FROM dolt_status}), '1', 'the table reads as changed');

# Off again: no more naps.
set_conf('prolly_fold_worker_cost_delay', '0');
fold_writes('off again', 'unpaced again');
is(counter('fold_worker_cost_delays'), $naps, 'unpaced again, no more naps');
is(sql(q{SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::s.t)}),
	'500', 'the working set is the last writes');

# A fold paced to take minutes (100 ms naps at every delay point): one in
# a nap, waited for.
set_conf('prolly_fold_worker_cost_delay', '100ms');
my $napping = q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE application_name = 'prolly fold worker'
	AND wait_event = 'VacuumDelay'};

# It gives way to a lock request on what it holds: the lock is had at
# once, not after the fold; the fold is given up as a retry, and made
# after.
my $retries = counter('fold_worker_retries');
sql(q{UPDATE s.t SET v = 'locked' WHERE id <= 500});
ok($node->poll_query_until('postgres', $napping), 'a paced fold naps');
my $t0 = time;
is( sql(q{
	SET lock_timeout = '20s';
	BEGIN;
	LOCK TABLE s.t IN ACCESS EXCLUSIVE MODE;
	COMMIT;
	SELECT 'locked'}), 'locked', 'the lock is had');
cmp_ok(time - $t0, '<', 8, 'promptly');
ok( $node->poll_query_until(
		'postgres',
		qq{SELECT value > $retries FROM pg_stat_prolly
		   WHERE name = 'fold_worker_retries'}),
	'the fold gave way, a retry');

# A paced fold naps 10 s in all at most, then goes on unpaced: the writes
# are folded long before 100 ms naps would let them be.
ok($node->poll_query_until('postgres', $folded), 'folded after giving way');
sql(q{UPDATE s.t SET v = 'capped' WHERE id <= 500});
$t0 = time;
ok($node->poll_query_until('postgres', $napping), 'napping again');
ok($node->poll_query_until('postgres', $folded), 'folded');
cmp_ok(time - $t0, '<', 40, 'within its naps\' cap');

# A reload reaches the fold under way: pacing turned off, it ends at once.
sql(q{UPDATE s.t SET v = 'reloaded' WHERE id <= 500});
ok($node->poll_query_until('postgres', $napping), 'napping once more');
$t0 = time;
set_conf('prolly_fold_worker_cost_delay', '0');
ok($node->poll_query_until('postgres', $folded), 'folded after the reload');
cmp_ok(time - $t0, '<', 6, 'at once, not after its naps');

# A limit of 0 means nothing, and is refused.
my ($ret, $out, $err) = $node->psql('postgres',
	q{ALTER SYSTEM SET prolly_fold_worker_cost_limit = 0});
like($err, qr/must be -1 or between 1 and 10000/, 'a limit of 0 is refused');

$node->stop;
done_testing();
