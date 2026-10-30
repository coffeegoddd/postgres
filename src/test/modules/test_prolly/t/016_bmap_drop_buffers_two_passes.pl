# Copyright (c) 2026, PostgreSQL Global Development Group

# Dropping every buffer of a relation drops its map fork's last.  Writing out
# a block-mapped page reads the relation's map to find where the page goes,
# so a page being written out while the relation is dropped must still find
# the map's pages in their buffers: the buffer pool scan that drops a
# dropped relation's buffers passes the map's by, drops the rest (waiting
# for a page being written out), and only then drops the map's.
#
# A standby replaying a drop while a restartpoint writes out a page of the
# relation is where this matters: on a primary, the drop has its relation's
# pages dropped before its blocks are freed, but redo leaves that to the
# primary's records.  The restartpoint is stopped while it writes out the
# table's first page, the drop's replay then waits for that buffer, and the
# map's buffers, which the scan reaches first, must still be there when the
# write goes on.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $primary = PostgreSQL::Test::Cluster->new('bmap_drop_two_passes');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
shared_buffers = 16MB
checkpoint_timeout = 1h
wal_keep_size = '1GB'
});
$primary->start;
$primary->safe_psql('postgres',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points;
	 CREATE EXTENSION pg_buffercache');
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('bmap_drop_two_passes_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', q{
bgwriter_lru_maxpages = 0
});
$standby->start;

sub psql_standby { return $standby->safe_psql('postgres', $_[0]); }

sub catch_up
{
	$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));
}

# A table larger than the drop's lookups of its blocks one by one would
# take (shared_buffers / 32), so the drop scans the whole buffer pool.  The
# checkpoint gives the standby a restartpoint to make, after the table.
$primary->safe_psql('postgres', q{
	CREATE TABLE t (a int, b int8) WITH (fillfactor = 10);
	INSERT INTO t SELECT g, g FROM generate_series(1, 4000) g;
	CHECKPOINT});
my $relnode = $primary->safe_psql('postgres', q{SELECT pg_relation_filenode('t')});
cmp_ok($primary->safe_psql('postgres', q{SELECT pg_relation_size('t') / 8192}),
	'>', 2048 / 32, 'the table is too large to drop block by block');
catch_up();

# On the standby, the map's first page has a buffer the scan reaches before
# the table's first page.
my $bufferid = qq{SELECT bufferid FROM pg_buffercache
	WHERE relfilenode = $relnode AND relblocknumber = 0 AND relforknumber = };
my $map_buffer = psql_standby($bufferid . '4');
my $page_buffer = psql_standby($bufferid . '0');
cmp_ok($map_buffer, '<', $page_buffer,
	'the map\'s buffer comes before the first page\'s in the buffer pool');
is( psql_standby(
		qq{SELECT isdirty FROM pg_buffercache WHERE bufferid = $page_buffer}),
	't', 'and the page is dirty, for the restartpoint to write out');

# The restartpoint, stopped writing out the table's first page.
psql_standby(q{SELECT injection_points_attach('bmap-write', 'wait')});
my $log_offset = -s $standby->logfile;
my $checkpoint = $standby->background_psql('postgres', on_error_stop => 0);
$checkpoint->query_until(qr/running/, q{
	\echo running
	CHECKPOINT;
	\echo checkpointed
});
$standby->wait_for_event('checkpointer', 'bmap-write');
psql_standby(q{SELECT injection_points_detach('bmap-write')});

# The drop's replay waits for the page being written out.
$primary->safe_psql('postgres', 'DROP TABLE t');
ok( $standby->poll_query_until(
		'postgres', q{SELECT wait_event FROM pg_stat_activity
			WHERE backend_type = 'startup'}, 'BufferIo'),
	'the drop\'s replay waits for the page being written out');
is(psql_standby($bufferid . '4'), $map_buffer,
	'with the map\'s buffer still there');

# The write goes on, finding the map; then the drop's replay drops it.
psql_standby(q{SELECT injection_points_wakeup('bmap-write')});
$checkpoint->{timeout}->start(10 * $PostgreSQL::Test::Utils::timeout_default);
$checkpoint->query_until(qr/checkpointed/, '');
unlike($checkpoint->{stderr}, qr/ERROR/, 'the restartpoint succeeds');
$checkpoint->quit;
my $log = slurp_file($standby->logfile, $log_offset);
unlike($log, qr/ERROR|PANIC/, 'with no error writing the page out');
catch_up();
is(psql_standby(qq{SELECT count(*) FROM pg_buffercache WHERE relfilenode = $relnode}),
	'0', 'and the table\'s buffers are all dropped');

$standby->stop;
$primary->stop;
done_testing();
