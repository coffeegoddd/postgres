# Copyright (c) 2026, PostgreSQL Global Development Group

# A block-mapped relation's truncation writes XLOG_SMGR_TRUNCATE, flushed
# before the buffers past the new length are dropped, and then the map
# header's own record, which nothing flushes.  A crash between the two
# leaves only the first durable.  Its replay drops those buffers too, so it
# must cut the header itself: left long, the table would read its pages
# past the length from disk, as they were before the records replayed onto
# them, and rows deleted before the truncation would come back.  A folded
# relation lists the truncation on its changes list, as the header's record
# does.  A standby replays the same.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('truncation_record');
$node->init(allows_streaming => 1);
# (the pages deleted from stay in shared buffers, unwritten, until the crash)
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
shared_buffers = 16MB
wal_consistency_checking = 'all'
});
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('truncation_record_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub nblocks
{
	my ($n, $rel) = @_;
	return $n->safe_psql('postgres',
		qq{SELECT pg_relation_size('$rel') / current_setting('block_size')::int});
}

sub past_end
{
	my ($n, $rel) = @_;
	return $n->safe_psql('postgres',
		qq{SELECT count(*) FROM test_bmap_past_end(pg_relation_filenode('$rel'))});
}

# Delete all but the first 10 blocks' rows of a new 100-block table and
# vacuum it, crashing once its XLOG_SMGR_TRUNCATE is flushed, before the
# header's record is written.  The vacuum's backend alone waits.
sub crash_mid_truncation
{
	my ($rel, $before_delete) = @_;
	sql(qq{CREATE TABLE $rel (a int, b text);
		INSERT INTO $rel SELECT g, repeat('x', 500) FROM generate_series(1, 1500) g});
	is(nblocks($node, $rel), '100', "$rel has 100 blocks");
	sql('CHECKPOINT');
	$before_delete->() if $before_delete;
	sql(qq{DELETE FROM $rel WHERE a > 150});

	my $vacuum = $node->background_psql('postgres');
	$vacuum->query_safe(q{SELECT injection_points_set_local()});
	$vacuum->query_safe(
		q{SELECT injection_points_attach('bmap-truncating', 'wait')});
	$vacuum->query_until(qr/vacuuming/, qq{\\echo vacuuming
VACUUM $rel;
});
	$node->wait_for_event('client backend', 'bmap-truncating');
	$node->stop('immediate');
	$vacuum->{run}->finish;
	$node->start;
}

sub standby_agrees
{
	my ($what, $rel) = @_;
	$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
	my $q = qq{SELECT count(*), sum(a) FROM $rel};
	is($standby->safe_psql('postgres', $q), sql($q),
		"$what: the standby reads $rel as the primary does");
	is(nblocks($standby, $rel), nblocks($node, $rel),
		"$what: with the same length");
	is(past_end($standby, $rel), past_end($node, $rel),
		"$what: and maps as much past it");
}

# A table.
crash_mid_truncation('h');
is(nblocks($node, 'h'), '10',
	'after a crash between the two records, the table has the length it was cut to');
is(sql(q{SELECT count(*), sum(a) FROM h}), '150|11325',
	'and only the rows it kept');
is(past_end($node, 'h'), '90', 'the blocks past its length stay mapped');
standby_agrees('a crash between the records', 'h');
sql(q{DELETE FROM h WHERE a > 60; VACUUM h});
is(nblocks($node, 'h'), '4', 'the next truncation cuts it again');
is(past_end($node, 'h'), '0', 'and lets go of every block past it');
standby_agrees('the next truncation', 'h');

# A folded table lists the truncation.
my ($rel, $lineage, $epoch);
crash_mid_truncation(
	'f',
	sub {
		$rel = sql(q{SELECT pg_relation_filenode('f')});
		($lineage, $epoch) =
		  split /\|/, sql(qq{SELECT lineage, epoch FROM test_bmap_fold_begin($rel)});
	});
is(nblocks($node, 'f'), '10', 'a folded table has the length it was cut to');
is(sql(q{SELECT count(*), sum(a) FROM f}), '150|11325', 'and its rows');
my $truncations = qq{SELECT string_agg(blkno::text, ' ' ORDER BY blkno)
	FROM test_bmap_fold_changes($rel, $lineage, $epoch) WHERE kind = 'truncate'};
is(sql($truncations), '10', 'its changes list has the truncation');
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $truncations), '10', 'on the standby too');
standby_agrees('a folded table', 'f');

$standby->stop;
$node->stop;
done_testing();
