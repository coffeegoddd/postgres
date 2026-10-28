# Copyright (c) 2026, PostgreSQL Global Development Group

# A checkpoint's save of a database's journal index holds the index's lock
# only to take the root and the segments' positions, never across the
# file's writes: stopped as it writes the chunks, it holds up no backend
# writing or reading chunks (each needs the lock, exclusive, to index what
# the journal has since), nor a backend's first use of the index.  And the
# file it then puts in place is complete: after a crash right after the
# checkpoint, every chunk indexed before it began is found from the file,
# with hardly any journal read, and those written while it ran are found
# from the journal.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('jindex_unlocked');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE ix');
$node->safe_psql('ix', 'CREATE EXTENSION test_prolly');
$node->safe_psql('ix', 'CREATE EXTENSION injection_points');

sub sql { return $node->safe_psql('ix', $_[0]); }

my $dir = $node->data_dir . '/base/'
  . $node->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'ix'});
my $file = "$dir/prolly_index";

sub put
{
	my ($tag, $n) = @_;
	return sql(qq{SELECT count(test_chunk_put(test_noise('$tag ' || g, 20000)))
		FROM generate_series(1, $n) g});
}

sub found
{
	my ($tag, $n) = @_;
	return qq{SELECT count(*) FROM generate_series(1, $n) g
		WHERE test_chunk_has(test_content_hash(test_noise('$tag ' || g, 20000)))};
}

put('old', 100);
sql('CHECKPOINT');
ok(-e $file, 'a checkpoint saves the index');
put('before', 50);
# (read by another backend: indexed in the shared index, for the next
# checkpoint to save)
is(sql(found('before', 50)), '50', 'chunks written after the first checkpoint');

# The checkpointer stopped as it writes the chunks of the index it read.
sql(q{SELECT injection_points_attach('prolly-jindex-save-chunks', 'wait')});
my $cp = $node->background_psql('postgres');
$cp->query_until(qr/started/, q{
	\echo started
	CHECKPOINT;
	\echo done
});
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) = 1 FROM pg_stat_activity
		WHERE backend_type = 'checkpointer'
		  AND wait_event = 'prolly-jindex-save-chunks'}),
	'the checkpointer is stopped writing the index');

# Meanwhile: chunks written by a backend, and found by another using the
# index first, each in a session of its own; each finishes while the
# checkpointer is still stopped, never waiting on the index's lock.  (An
# LWLock wait can't be cancelled: a session that waits is watched for,
# for a while, then let go of by waking the checkpointer.)
my $lock_waits = 0;
sub runs_beside_checkpoint
{
	my ($what, $query) = @_;
	my $s = $node->background_psql('ix');
	my $pid = $s->query_safe(q{SELECT pg_backend_pid()});
	$s->query_until(qr/started/, qq{
		\\echo started
		$query;
		\\echo done
	});
	my $finished = 0;
	for (1 .. 200)
	{
		my $state = $node->safe_psql('postgres', qq{SELECT state || ':' ||
			coalesce(wait_event, '') FROM pg_stat_activity WHERE pid = $pid});
		$lock_waits++ if $state =~ /prolly journal index/;
		if ($state =~ /^idle/)
		{
			$finished = 1;
			last;
		}
		select(undef, undef, undef, 0.05);
	}
	ok($finished, "$what, while the checkpointer writes the index");
	return $s;
}
my $writer = runs_beside_checkpoint('a backend writes chunks',
	q{SELECT 'put ' || count(test_chunk_put(test_noise('during ' || g, 20000)))
	FROM generate_series(1, 10) g});
my $reader = runs_beside_checkpoint('another, using the index first, finds them',
	q{SELECT 'found ' || count(*) FROM generate_series(1, 10) g
	WHERE test_chunk_has(test_content_hash(test_noise('during ' || g, 20000)))});
is($lock_waits, 0, 'neither waited on the index\'s lock');
is( $node->safe_psql('postgres', q{SELECT count(*) FROM pg_stat_activity
		WHERE backend_type = 'checkpointer'
		  AND wait_event = 'prolly-jindex-save-chunks'}),
	'1', 'the checkpointer was still writing the index all the while');

sql(q{SELECT injection_points_detach('prolly-jindex-save-chunks');
	SELECT injection_points_wakeup('prolly-jindex-save-chunks')});
like($writer->query_until(qr/done/, ''), qr/put 10/, 'all the chunks were written');
like($reader->query_until(qr/done/, ''), qr/found 10/, 'and all found');
$writer->quit;
$reader->quit;
$cp->query_until(qr/done/, '');
$cp->quit;

# A crash right after: the file saved has every chunk indexed before the
# checkpoint began; those written while it ran are read from the journal.
sub scanned
{
	my ($query) = @_;
	my @out = split /\n/, sql(qq{SELECT pg_stat_reset_shared('prolly');
		SELECT ($query) IS NOT NULL;
		SELECT pg_stat_force_next_flush();
		SELECT value FROM pg_stat_prolly WHERE name = 'journal_bytes_scanned'});
	return $out[-1];
}
$node->stop('immediate');
$node->start;
cmp_ok(scanned(found('before', 50)), '<', 600000,
	'after a crash, the chunks indexed before the checkpoint are found from its file');
is(sql(found('old', 100)), '100', 'every chunk saved before it');
is(sql(found('before', 50)), '50', 'every chunk written just before it');
is(sql(found('during', 10)), '10', 'and those written while it ran, from the journal');

$node->stop;
done_testing();
