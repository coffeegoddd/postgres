# Copyright (c) 2026, PostgreSQL Global Development Group

# A checkpoint fsyncs the chunk stores' files written since the last one,
# by the sync requests their writers made, not every file of every store.
# A request the checkpointer's queue refuses has the next checkpoint fsync
# them all instead.  A file a collection removed meanwhile is no error.
# What a checkpoint fsynced survives a crash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('ckpt_sync');
$node->init;
# (with fsync off, a checkpoint drops its sync requests unread)
$node->append_conf('postgresql.conf', qq{
autovacuum = off
fsync = on
log_min_messages = notice
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
for my $db ('one', 'two', 'three')
{
	$node->safe_psql('postgres', "CREATE DATABASE $db");
	$node->safe_psql($db, 'CREATE EXTENSION test_prolly');
	$node->safe_psql($db, qq{SELECT test_chunk_put('$db first')});
}
$node->safe_psql('three', q{SELECT test_chunk_table_file(ARRAY['in a file'::bytea])});
$node->safe_psql('postgres', 'CHECKPOINT');

sub sql { return $node->safe_psql('postgres', $_[0]); }

# The notices a checkpoint's injection points gave, from here on.
my $offset = -s $node->logfile;
sub checkpoint_notices
{
	my ($point) = @_;
	sql('CHECKPOINT');
	my $log = slurp_file($node->logfile, $offset);
	$offset = -s $node->logfile;
	my @n = ($log =~ /notice triggered for injection point $point/g);
	return scalar(@n);
}

sql(q{SELECT injection_points_attach('prolly-checkpoint-chunk-file', 'notice')});
sql(q{SELECT injection_points_attach('prolly-checkpoint-table-file', 'notice')});
is(checkpoint_notices('prolly-checkpoint-chunk-file'), 0,
	'a checkpoint after nothing was written fsyncs no store file');

$node->safe_psql('one', q{SELECT test_chunk_put('one second')});
is(checkpoint_notices('prolly-checkpoint-chunk-file'), 1,
	'one written since: only its segment is fsynced');
is(checkpoint_notices('prolly-checkpoint-chunk-file'), 0, 'and only once');

$node->safe_psql('two', q{SELECT test_chunk_put('two second')});
$node->safe_psql('three', q{SELECT test_chunk_put('three second')});
is(checkpoint_notices('prolly-checkpoint-chunk-file'), 2,
	'two stores written since: their two segments');

# A request refused: every store's files are fsynced, by the full scan
# (whose notices are the table files'), once.
sql(q{SELECT injection_points_attach('prolly-sync-request-refused', 'notice')});
$node->safe_psql('two', q{SELECT test_chunk_put('two refused')});
sql(q{SELECT injection_points_detach('prolly-sync-request-refused')});
is(checkpoint_notices('prolly-checkpoint-table-file'), 1,
	'a refused request: every store\'s files fsynced');
is(checkpoint_notices('prolly-checkpoint-table-file'), 0, 'then requests again');
# An append reserving its place as a checkpoint runs (its WAL record
# after the redo point, so the checkpoint doesn't wait for it): its sync
# request, made once it is written, is the next checkpoint's.  (One made
# before the write would be taken by this checkpoint, the segment fsynced
# without the bytes, and no checkpoint would fsync them.)
sql(q{SELECT injection_points_attach('prolly-append-reserving', 'wait')});
my $a = $node->background_psql('one');
$a->query_until(qr/started/, q{
	\echo started
	SELECT test_chunk_put('one held') IS NOT NULL;
	\echo done
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-append-reserving'}) or die 'the append did not wait';
is(checkpoint_notices('prolly-checkpoint-chunk-file'), 0,
	'a checkpoint as an append reserves fsyncs nothing of it');
sql(q{SELECT injection_points_detach('prolly-append-reserving')});
sql(q{SELECT injection_points_wakeup('prolly-append-reserving')});
$a->query_until(qr/done/, '');
$a->quit;
is(checkpoint_notices('prolly-checkpoint-chunk-file'), 1,
	'the next fsyncs its segment, written');
sql(q{SELECT injection_points_detach('prolly-checkpoint-chunk-file')});
sql(q{SELECT injection_points_detach('prolly-checkpoint-table-file')});

# A segment a collection retires while the checkpoint is about to fsync it.
$node->safe_psql('one', q{SELECT test_chunk_put('one retired')});
sql(q{SELECT injection_points_attach('prolly-checkpoint-chunk-file', 'wait')});
my $c = $node->background_psql('postgres');
$c->query_until(qr/started/, q{
	\echo started
	CHECKPOINT;
	\echo done
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-checkpoint-chunk-file'}) or die 'the checkpoint did not wait';
my $segments = q{SELECT string_agg(name, ' ' ORDER BY name) FROM test_chunk_segments()};
my $before = $node->safe_psql('one', $segments);
$node->safe_psql('one', 'SELECT chunks_freed > 0 FROM prolly_gc()');
isnt($node->safe_psql('one', $segments), $before, 'the collection retired the segment');
sql(q{SELECT injection_points_detach('prolly-checkpoint-chunk-file')});
sql(q{SELECT injection_points_wakeup('prolly-checkpoint-chunk-file')});
$c->query_until(qr/done/, '');
$c->quit;
is(sql('SELECT 1'), '1', 'the checkpoint completes');

# A table file a conjoin removes while the checkpoint is about to fsync it.
$node->safe_psql('three', q{SELECT test_chunk_table_file(ARRAY['conjoined'::bytea])});
sql(q{SELECT injection_points_attach('prolly-checkpoint-chunk-file', 'wait')});
$c = $node->background_psql('postgres');
$c->query_until(qr/started/, q{
	\echo started
	CHECKPOINT;
	\echo done
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-checkpoint-chunk-file'}) or die 'the checkpoint did not wait';
$node->safe_psql('three', 'SELECT test_chunk_conjoin()');
sql(q{SELECT injection_points_detach('prolly-checkpoint-chunk-file')});
sql(q{SELECT injection_points_wakeup('prolly-checkpoint-chunk-file')});
$c->query_until(qr/done/, '');
$c->quit;
is($node->safe_psql('three', q{SELECT convert_from(test_chunk_get(test_content_hash('conjoined')), 'UTF8')}),
	'conjoined', 'a checkpoint racing a conjoin completes, and the chunk reads');

# What was written before a checkpoint, and after, survives a crash.
$node->safe_psql('two', q{SELECT test_chunk_put('two before')});
sql('CHECKPOINT');
$node->safe_psql('two', q{SELECT test_chunk_put('two after')});
$node->stop('immediate');
$node->start;
is($node->safe_psql('two', q{SELECT convert_from(test_chunk_get(test_content_hash('two before')), 'UTF8')
	|| ', ' || convert_from(test_chunk_get(test_content_hash('two after')), 'UTF8')}),
	'two before, two after', 'both survive a crash');

$node->stop;
done_testing();
