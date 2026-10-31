# Copyright (c) 2026, PostgreSQL Global Development Group

# A block copied from without an image is changed in place or freed, once
# the restart point is past the copy, after a PASSED record, unless one
# said a copy no older is passed already (copies_passed()).  pg_control
# keeps the newest such position, noted with each; a restart doesn't forget
# it, so a block whose copy is no newer than it needs no PASSED record
# again, nor a note that a pg_rewind copying files would take for blocks
# synced while it copied.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('passed_restart');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
block_map_copy_images = off
block_map_standby_hold_max_size = 0
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 1GB
bgwriter_lru_maxpages = 0
wal_writer_delay = 10s
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_walinspect;
});

sub sql { return $node->safe_psql('postgres', $_[0]); }

# A table shared, then changed everywhere, its pages copied without images
# (each changed since the checkpoint): the copy holds the old blocks.
sub shared_and_copied
{
	my ($name) = @_;
	sql(qq{
		CREATE TABLE $name (a int, b text) WITH (fillfactor = 50);
		INSERT INTO $name SELECT g, repeat('x', 200) FROM generate_series(1, 20 * 17) g;
		CHECKPOINT;
		UPDATE $name SET a = a WHERE a % 17 = 1;
	});
	my $copy = sql(qq{SELECT test_bmap_share(pg_relation_filenode('$name'))});
	sql(qq{UPDATE $name SET b = 'changed'});
	return $copy;
}

sub passed_since
{
	my ($start) = @_;
	return sql(qq{
		SELECT count(*) FROM pg_get_wal_records_info('$start', pg_current_wal_insert_lsn())
		WHERE resource_manager = 'BlockMap' AND record_type = 'PASSED'});
}

sub synced_count
{
	my ($stdout) = run_command([ 'pg_controldata', $node->data_dir ]);
	$stdout =~ /Times pages synced unlogged: +(\d+)/ or die "no synced count";
	return $1;
}

# The copy holds the table's old blocks alone, copied from without
# images.  After a checkpoint past the copies, the copy's first changes
# claim its blocks in place: a PASSED record first, and the copies' newest
# position (the count page's) noted.
my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
my $copy = shared_and_copied('a');
my $pages = sql(q{SELECT pg_relation_size('a') / 8192});
sql(q{CHECKPOINT});
sql(qq{SELECT test_bmap_write($copy, b, 7) FROM generate_series(0, $pages / 2 - 1) b});
sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
cmp_ok(passed_since($start), '>=', 1, 'changing in place a block copied from logs a PASSED record');

# After a restart, the copy's other pages changed in place: on the same
# count page, their copy is the position noted, so nothing is logged or
# noted again.
$node->restart;
my $count = synced_count();
$start = sql(q{SELECT pg_current_wal_insert_lsn()});
sql(qq{SELECT test_bmap_write($copy, b, 8) FROM generate_series($pages / 2, $pages - 1) b});
sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
is(passed_since($start), '0', 'after a restart, changing the others logs no PASSED record again');
$node->stop;
is(synced_count(), $count, 'nor a note in pg_control');

# The note stands for its record, so the record is flushed before it: a
# crash right after the note (the WAL writer slow, the change's transaction
# not committed) leaves both, never the note alone.
SKIP:
{
	skip 'Injection points not supported by this build', 2
	  if $ENV{enable_injection_points} ne 'yes';

	$node->start;
	sql(q{CREATE EXTENSION injection_points});
	my $copy_c = shared_and_copied('c');
	my $cpages = sql(q{SELECT pg_relation_size('c') / 8192});
	sql(q{CHECKPOINT});
	my $before = synced_count();
	$start = sql(q{SELECT pg_current_wal_insert_lsn()});
	sql(q{SELECT injection_points_attach('bmap-copies-passed-noted', 'wait')});
	my $session = $node->background_psql('postgres', on_error_stop => 0);
	$session->query_until(qr/started/, qq{
		\\echo started
		BEGIN;
		SELECT test_bmap_write($copy_c, b, 9) FROM generate_series(0, $cpages - 1) b;
	});
	$node->wait_for_event('client backend', 'bmap-copies-passed-noted');
	$node->stop('immediate');
	eval { $session->quit };
	cmp_ok(synced_count(), '>', $before, 'the crash came after a note');
	$node->start;
	cmp_ok(passed_since($start), '>=', 1, 'and the PASSED record the note stands for survived it');
	$node->stop;
}

done_testing();
