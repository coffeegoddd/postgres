# Copyright (c) 2026, PostgreSQL Global Development Group

# The history window's note of the background's last collection (what the
# next seal is counted from) is kept outside the chunk store and the
# refs.  Kept in the store, it would itself be new data: an idle database
# would seal at every batch boundary, a generation and a table file each
# time.  Kept in the refs, the background would wait, after each
# collection, for a transaction holding them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

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
$node->safe_psql('ws', q{CREATE EXTENSION test_prolly;
	ALTER DATABASE ws SET prolly.history_batch = '2 seconds'});

sub sql { return $node->safe_psql('ws', $_[0]); }

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

# A transaction holding every ref: the background's seals still run, and
# it waits for no lock after them.
my $held = $node->background_psql('ws');
$held->query_safe(q{BEGIN; SELECT dolt_branch('held')});
my $waits = q{SELECT count(*) FROM pg_stat_activity
	WHERE backend_type = 'autovacuum worker' AND wait_event_type = 'Lock'};
my $seen_wait = 0;
my $collected = 0;
for (my $i = 0; $i < 3; $i++)
{
	my $chunk = sql(qq{SELECT test_chunk_put('sealed while held $i')});
	my $deadline = time() + 20;
	my $gone = 0;
	while (time() < $deadline)
	{
		$seen_wait ||= sql($waits) > 0;
		if (sql(qq{SELECT NOT test_chunk_has('$chunk')}) eq 't')
		{
			$gone = 1;
			last;
		}
		usleep(100_000);
	}
	$collected++ if $gone;
	# (a while after, as the worker would end its step)
	for (my $j = 0; $j < 10; $j++)
	{
		$seen_wait ||= sql($waits) > 0;
		usleep(100_000);
	}
}
is($collected, 3, 'seals collect while another transaction holds the refs');
is($seen_wait, 0, 'and the background waits for no lock after them');
$held->query_safe('COMMIT');
$held->quit;

$node->stop;
done_testing();
