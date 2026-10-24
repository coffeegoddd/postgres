# Copyright (c) 2026, PostgreSQL Global Development Group

# prolly_fold_worker_cost_delay: the fold worker's folds paced by the
# cost-based delay, as vacuum is.  Off (the default), a fold naps never;
# on, a fold that has spent prolly_fold_worker_cost_limit naps
# (fold_worker_cost_delays counts the naps), and folds the same rows.  Off
# again, it naps no more: nothing of the pacing outlives the folds paced.

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

$node->stop;
done_testing();
