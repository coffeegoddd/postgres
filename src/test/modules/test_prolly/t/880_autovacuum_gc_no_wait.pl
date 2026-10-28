# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum's collections of the chunk store (collect, retire, move) wait
# for a transaction at most prolly.gc_free_wait, polling for its end, never
# sleeping on its lock: one left open would otherwise hold the worker, and
# every collection after it, for as long as it stays open.
# - A transaction using the store as a collection begins, still running at
#   the bound, gives the collection up before it seals anything (no
#   segment is left behind), counted as skipped; it runs once that ends.
# - A steady stream of short transactions doesn't hold it off: only those
#   using the store as it begins are waited for, a set that only shrinks.
# - A move of the journal is given up alike.
# - A transaction using the store at the swap, still running after the
#   retire's wait, leaves the files the swap dropped retiring; a later
#   collection removes them once it has ended, under load all the same.

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
prolly.gc_free_wait = 500ms
prolly_fold_worker = off
log_min_messages = debug1
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE av');
$node->safe_psql('av',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
my $dir = $node->data_dir . '/base/'
  . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'av'});

sub sql { return $node->safe_psql('av', $_[0]); }
sub has { return sql("SELECT test_chunk_has('$_[0]')"); }

sub garbage
{
	sql(qq{SELECT count(test_chunk_put(test_noise(g::text || '$_[0]', 100000)))
		FROM generate_series(1, 20) g});
}

sub segments
{
	opendir(my $d, $dir) or die "opendir $dir: $!";
	my @s = sort grep { /^prolly_journal_/ } readdir($d);
	closedir($d);
	return @s;
}

sub log_count
{
	my ($offset, $re) = @_;
	my @m = (slurp_file($node->logfile, $offset) =~ /$re/g);
	return scalar(@m);
}

# Until the log has matched re n times since offset, sampling whether an
# autovacuum worker sleeps on a lock meanwhile: 1 if one did.
sub lock_waits_until
{
	my ($offset, $re, $n) = @_;
	my $w = 0;
	my $tries = 0;
	while (log_count($offset, $re) < $n)
	{
		$w = 1
		  if $node->safe_psql('postgres', q{SELECT count(*) FROM pg_stat_activity
			WHERE backend_type = 'autovacuum worker' AND wait_event_type = 'Lock'}) > 0;
		usleep(50_000);
		die "the log never matched $re $n times" if ++$tries > 1200;
	}
	return $w;
}

my $collected_re = qr/automatic collection of database "av": \d+ chunks kept/;

# Is chunk h collected by the collections from offset on?  Asked only once
# one has finished (a chunk has() finds while a collection runs is kept by
# it): after each, once; if it's still there, more garbage brings another
# collection, up to a few times.
sub collected_after
{
	my ($offset, $h, $tag) = @_;
	for my $round (1 .. 5)
	{
		my $tries = 0;
		while (log_count($offset, $collected_re) < $round)
		{
			usleep(50_000);
			return 0 if ++$tries > 1200;
		}
		return 1 if has($h) eq 'f';
		garbage(" $tag $round");
	}
	return 0;
}

my $given_up =
  qr/automatic collection of database "av" given up: a transaction it would wait for is running/;

# An open transaction that used the store, then garbage past the
# threshold: each collection waits for it to its bound, never on its
# lock, and gives up before it seals: no segment is added.
my $holder = $node->background_psql('av');
$holder->query_safe(q{BEGIN});
$holder->query_safe(q{SELECT test_chunk_has(test_chunk_put('the holder''s'))});
my $dead = sql(q{SELECT test_chunk_put('dead while a reader is open')});
garbage(' one');
my @before = segments();
my $offset = -s $node->logfile;
is(lock_waits_until($offset, $given_up, 3), 0,
	'three collections given up, none sleeping on the open transaction\'s lock');
is(join(',', segments()), join(',', @before),
	'the collections given up sealed no segment');
is(has($dead), 't', 'the garbage stays meanwhile');
$offset = -s $node->logfile;
$holder->query_safe(q{COMMIT});
ok(collected_after($offset, $dead, 'one more'),
	'and is collected once the transaction ends');

# A steady stream of short transactions reading the store, in two
# sessions: a collection runs all the same.
sql(q{CREATE PROCEDURE busy(n int) LANGUAGE plpgsql AS $$
	DECLARE
		h text := test_chunk_put('busy');
	BEGIN
		FOR i IN 1..n LOOP
			PERFORM test_chunk_has(h);
			PERFORM pg_sleep(0.05);
			COMMIT;
		END LOOP;
	END $$});
my @busy;
for (1 .. 2)
{
	my $b = $node->background_psql('av');
	$b->query_until(qr/busy/, q{\echo busy
		CALL busy(100000);
});
	push @busy, $b;
}
my $dead_busy = sql(q{SELECT test_chunk_put('dead under load')});
$offset = -s $node->logfile;
garbage(' two');
ok(collected_after($offset, $dead_busy, 'two more'),
	'under a steady stream of short transactions, a collection runs');

# A transaction begun after a collection's waits, using the store at its
# swap and after: the retire waits for it to its bound, then leaves the
# segments the swap dropped retiring; what was collected is gone.
$node->safe_psql('av',
	q{SELECT injection_points_attach('prolly-gc-running-waited', 'wait')});
my $dead2 = sql(q{SELECT test_chunk_put('dead at a reader''s swap')});
garbage(' three');
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE backend_type = 'autovacuum worker'
		AND wait_event = 'prolly-gc-running-waited'}),
	'a collection waits at the injection point');
my @collected = segments();
pop @collected;    # (the one appends go to since the seal)
my $reader = $node->background_psql('av');
$reader->query_safe(q{BEGIN});
$reader->query_safe(q{SELECT test_chunk_has(test_chunk_put('the reader''s'))});
$offset = -s $node->logfile;
$node->safe_psql('av', q{
	SELECT injection_points_detach('prolly-gc-running-waited');
	SELECT injection_points_wakeup('prolly-gc-running-waited')});
is(lock_waits_until($offset, $collected_re, 1), 0,
	'its retire sleeps on no lock of a transaction using the store at the swap');
is(has($dead2), 'f', 'what it collected is gone');
my %now = map { $_ => 1 } segments();
ok(scalar(grep { $now{$_} } @collected) > 0,
	'the segments it dropped stay, retiring, while the transaction runs');

# Once it ends, under the same load, a later collection removes them.
$reader->query_safe(q{COMMIT});
$reader->quit;
garbage(' four');
my $gone = 0;
for (1 .. 600)
{
	%now = map { $_ => 1 } segments();
	if (!grep { $now{$_} } @collected)
	{
		$gone = 1;
		last;
	}
	usleep(100_000);
}
ok($gone, 'a later collection removes the retiring segments, under load');

# A move of the journal: with collections off and the move due, an open
# transaction that may have appended (one with an xid) gives the move up
# too.  (One that only read doesn't: a move frees nothing; t/069.)
$node->append_conf('postgresql.conf', qq{
prolly.gc_journal_threshold = -1
prolly.journal_move_threshold = 1
});
$node->reload;
$node->poll_query_until('av',
	q{SELECT current_setting('prolly.journal_move_threshold') = '1MB'})
  or die 'the setting did not reload';
$holder->query_safe(q{BEGIN});
$holder->query_safe(q{SELECT txid_current()});
$holder->query_safe(q{SELECT test_chunk_has(test_chunk_put('the holder''s again'))});
garbage(' five');
$offset = -s $node->logfile;
is( lock_waits_until(
		$offset,
		qr/automatic move of the chunk journal of database "av" given up/, 2),
	0,
	'a move is given up too, sleeping on no lock');
$holder->query_safe(q{COMMIT});
my $moved = 0;
for (1 .. 600)
{
	if (log_count($offset, qr/automatic move of the chunk journal of database "av": \d+ chunks moved/))
	{
		$moved = 1;
		last;
	}
	usleep(100_000);
}
ok($moved, 'and the journal is moved once it ends');

$node->safe_psql('av', q{SELECT pg_terminate_backend(pid) FROM pg_stat_activity
	WHERE query LIKE 'CALL busy%'});
eval { $_->quit } for @busy;
$holder->quit;
$node->stop;
done_testing();
