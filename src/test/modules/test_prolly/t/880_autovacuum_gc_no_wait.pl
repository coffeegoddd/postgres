# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum's collection of the chunk store never waits for a
# transaction: one left open would hold the worker, and every collection
# after it, for as long as it stays open.  Where the collection would wait
# for one still running (one that used the store before the seal), it is
# given up, counted as skipped, and runs again on a later visit; and a
# transaction running at its swap, still running at the retire, leaves
# what the swap dropped retiring, for the next collection to remove.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('gc_no_wait');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = on
autovacuum_naptime = 1s
prolly.gc_journal_threshold = 1MB
prolly.gc_growth_scale_factor = 0
prolly_fold_worker = off
log_min_messages = debug1
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE av');
$node->safe_psql('av',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');

sub sql { return $node->safe_psql('av', $_[0]); }
sub has { return sql("SELECT test_chunk_has('$_[0]')"); }

# Did an autovacuum worker wait for a lock in the next few seconds?
sub waited
{
	my $w = 0;
	for (1 .. 40)
	{
		$w = 1
		  if $node->safe_psql('postgres', q{SELECT count(*) FROM pg_stat_activity
			WHERE backend_type = 'autovacuum worker' AND wait_event_type = 'Lock'}) > 0;
		usleep(100_000);
	}
	return $w;
}

sub garbage
{
	sql(qq{SELECT count(test_chunk_put(test_noise(g::text || '$_[0]', 100000)))
		FROM generate_series(1, 20) g});
}

# An open transaction that used the store, then garbage past the
# threshold: the collection, which would wait for the transaction, is
# given up, and the garbage stays until it ends.
my $holder = $node->background_psql('av');
$holder->query_safe(q{BEGIN});
$holder->query_safe(q{SELECT test_chunk_has(test_chunk_put('the holder''s'))});
my $dead = sql(q{SELECT test_chunk_put('dead while a reader is open')});
my $offset = -s $node->logfile;
garbage(' one');
is(waited(), 0, 'autovacuum waits for no transaction the collection would');
like(slurp_file($node->logfile, $offset),
	qr/automatic collection of database "av" given up: a transaction it would wait for is running/,
	'it gives the collection up');
is(has($dead), 't', 'the garbage stays meanwhile');
$holder->query_safe(q{COMMIT});
ok($node->poll_query_until('av', qq{SELECT NOT test_chunk_has('$dead')}),
	'and is collected once the transaction ends');

# A transaction begun after the collection's waits, running at its swap
# and after: the retire leaves what the swap dropped retiring rather than
# wait for it; what was collected is gone all the same, and the next
# collection runs once the transaction ends.
$node->safe_psql('av',
	q{SELECT injection_points_attach('prolly-gc-running-waited', 'wait')});
my $dead2 = sql(q{SELECT test_chunk_put('dead at a reader''s swap')});
garbage(' two');
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE backend_type = 'autovacuum worker'
		AND wait_event = 'prolly-gc-running-waited'}),
	'a collection waits at the injection point');
my $reader = $node->background_psql('av');
$reader->query_safe(q{BEGIN});
$reader->query_safe(q{SELECT 1});
$node->safe_psql('av', q{
	SELECT injection_points_detach('prolly-gc-running-waited');
	SELECT injection_points_wakeup('prolly-gc-running-waited')});
is(waited(), 0, 'its retire waits for no transaction running at the swap');
is(has($dead2), 'f', 'what it collected is gone');
$reader->query_safe(q{COMMIT});
$reader->quit;
my $dead3 = sql(q{SELECT test_chunk_put('dead after the reader')});
garbage(' three');
ok($node->poll_query_until('av', qq{SELECT NOT test_chunk_has('$dead3')}),
	'the next collection runs once it ends');

$holder->quit;
$node->stop;
done_testing();
