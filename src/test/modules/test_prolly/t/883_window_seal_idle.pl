# Copyright (c) 2026, PostgreSQL Global Development Group

# The history window's note of the background's last collection (what the
# next seal is counted from) is kept outside the chunk store and the
# refs.  Kept in the store, it would itself be new data: an idle database
# would seal at every batch boundary, a generation and a table file each
# time.  Kept in the refs, the background would wait, after each
# collection, for a transaction holding them (and with it every step of
# its visit after, and the commits queued behind it).

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

my $node = PostgreSQL::Test::Cluster->new('window_seal_idle');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = on
autovacuum_naptime = 1s
prolly.gc_journal_threshold = 1MB
prolly.gc_growth_scale_factor = 0
prolly_fold_worker = off
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE ws');
$node->safe_psql('ws', q{CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points;
	ALTER DATABASE ws SET prolly.history_batch = '2 seconds'});

sub sql { return $node->safe_psql('ws', $_[0]); }
sub has { return sql("SELECT test_chunk_has('$_[0]')"); }

my $generations = q{SELECT count(*) FROM test_chunk_epochs()};
my $files = q{SELECT count(*) FROM test_chunk_table_files()};

# A seal, then nothing written: the boundaries that pass make no more.
my $dead = sql(q{SELECT test_chunk_put('sealed once')});
ok($node->poll_query_until('ws', qq{SELECT NOT test_chunk_has('$dead')}),
	'a boundary\'s seal collects what was written');
my $gens = sql($generations);
my $nfiles = sql($files);
sleep(10);
is(sql($generations), $gens, 'an idle database seals no more generations');
is(sql($files), $nfiles, 'nor writes table files');

# A seal stopped after its swap; meanwhile a transaction holds every ref.
# Woken, the worker ends the seal (its note written) waiting for no lock.
$node->safe_psql('ws',
	q{SELECT injection_points_attach('prolly-gc-swapped', 'wait')});
my $held_dead = sql(q{SELECT test_chunk_put('sealed while held')});
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE backend_type = 'autovacuum worker'
		AND wait_event = 'prolly-gc-swapped'}),
	'a seal waits after its swap');
my $held = $node->background_psql('ws');
$held->query_safe(q{BEGIN; SELECT dolt_branch('held')});
$node->safe_psql('ws', q{
	SELECT injection_points_detach('prolly-gc-swapped');
	SELECT injection_points_wakeup('prolly-gc-swapped')});
my $waits = q{SELECT count(*) FROM pg_stat_activity
	WHERE backend_type = 'autovacuum worker' AND wait_event_type = 'Lock'};
my $seen_wait = 0;
my $gone = 0;
for (my $i = 0; $i < 100; $i++)
{
	$seen_wait = 1 if sql($waits) > 0;
	$gone = 1 if !$gone && has($held_dead) eq 'f';
	usleep(100_000);
}
is($seen_wait, 0, 'the worker waits for no lock after the seal');
is($gone, 1, 'and the seal collected');
$held->query_safe('COMMIT');
$held->quit;

$node->stop;
done_testing();
