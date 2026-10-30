# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_rewind and shares that sync their source's blocks instead of logging
# them.  A rewound server replays the source's WAL from the last common
# checkpoint over the page store copied from the source as it is now: if
# the source shared a table whose blocks it got after that checkpoint (a
# freed block reused, then a checkpoint, then the share), the replay of
# the block's allocation zeroes it again and the table's later records
# are skipped (its map, copied, is newer), so the copy would read zeroes.
# pg_rewind refuses such a source, leaving the target as it was, and the
# target can be made a standby again from a base backup.  A share of
# blocks the source got before that checkpoint leaves the rewind as it
# was: the replay begins after them.  Copies of shared pages made without
# their images, whose old blocks then change, are refused as such shares
# are.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Copy;
use IPC::Run;

sub sql { return $_[0]->safe_psql('postgres', $_[1]); }

my $rows_query = q{SELECT md5(string_agg(t::text, ',' ORDER BY a)) FROM t};

# A primary with a table 't', and a standby of it, promoted.
sub failover
{
	my ($name) = @_;
	my $primary = PostgreSQL::Test::Cluster->new("${name}_primary");
	$primary->init(allows_streaming => 1);
	$primary->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
wal_log_hints = on
wal_keep_size = 512MB
checkpoint_timeout = 1h
});
	$primary->start;
	sql($primary, q{
		CREATE EXTENSION test_prolly;
		CREATE EXTENSION pg_walinspect;
		CREATE TABLE t (a int, b text);
		INSERT INTO t SELECT g, 'before ' || g FROM generate_series(1, 2000) g;
		CHECKPOINT;
	});
	$primary->backup('base');
	my $standby = PostgreSQL::Test::Cluster->new("${name}_standby");
	$standby->init_from_backup($primary, 'base', has_streaming => 1);
	$standby->start;
	$primary->wait_for_replay_catchup($standby);
	$standby->promote;
	$standby->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
	  or die 'the standby was not promoted';
	return ($primary, $standby, sql($primary, q{SELECT pg_relation_filenode('t')}));
}

# The old primary diverges a little and stops; pg_rewind from the new one.
sub rewind
{
	my ($primary, $standby) = @_;

	sql($primary, q{INSERT INTO t VALUES (-1, 'old primary')});
	$primary->stop;
	my $conf = $primary->data_dir . '/postgresql.conf';
	my $kept = PostgreSQL::Test::Utils::tempdir . '/postgresql.conf';
	copy($conf, $kept) or die;
	my ($stdout, $stderr);
	my $ret = IPC::Run::run([
		'pg_rewind',
		'--source-server' => $standby->connstr('postgres'),
		'--target-pgdata' => $primary->data_dir,
		'--no-sync' ], '>', \$stdout, '2>', \$stderr);
	move($kept, $conf) or die;
	return ($ret, $stderr);
}

# The source shares t after t took blocks freed since the last common
# checkpoint, and a checkpoint, so the share syncs them; t changes after.
my ($primary, $standby, $t) = failover('rewind_synced');
sql($standby, q{
	CREATE TABLE freed (a int, b text);
	INSERT INTO freed SELECT g, repeat('f', 200) FROM generate_series(1, 4000) g;
	DROP TABLE freed;
});
sql($standby, q{SELECT * FROM test_bmap_release_drain()});
sql($standby, q{INSERT INTO t SELECT g, 'reused ' || g FROM generate_series(2001, 6000) g; CHECKPOINT;});
my $copy = sql($standby, qq{SELECT test_bmap_share($t)});
my $digest = sql($standby, qq{SELECT test_bmap_rows_digest($copy)});
sql($standby, q{UPDATE t SET b = 'changed' WHERE a % 10 = 1; CHECKPOINT;});
my $t_rows = sql($standby, $rows_query);
my $old_rows = sql($primary, $rows_query);

my ($ret, $stderr) = rewind($primary, $standby);
ok(!$ret, 'pg_rewind refuses a source that synced blocks given after the replay would begin');
like($stderr, qr/source server synced pages without logging them/,
	'saying why');

# The target is left as it was: it starts as the old primary.
$primary->start;
is(sql($primary, q{SELECT count(*) FROM t WHERE a = -1}), '1',
	'the target, untouched, starts with its own rows');
$primary->stop;

# A base backup of the source makes a standby that reads the copy.
$standby->backup('again');
my $clone = PostgreSQL::Test::Cluster->new('rewind_synced_clone');
$clone->init_from_backup($standby, 'again', has_streaming => 1);
$clone->start;
$standby->wait_for_replay_catchup($clone);
is(sql($clone, $rows_query), $t_rows, 'a standby made from a base backup reads t');
is(sql($clone, qq{SELECT test_bmap_rows_digest($copy)}), $digest,
	'and the copy');
$clone->stop;
$standby->stop;

# A share on the source of blocks t got before the last common checkpoint:
# the rewind goes ahead, and the rewound server reads the copy.
($primary, $standby, $t) = failover('rewind_synced_old');
sql($standby, q{CHECKPOINT});
$copy = sql($standby, qq{SELECT test_bmap_share($t)});
$digest = sql($standby, qq{SELECT test_bmap_rows_digest($copy)});
sql($standby, q{UPDATE t SET b = 'changed' WHERE a % 10 = 1; CHECKPOINT;});
$t_rows = sql($standby, $rows_query);
($ret, $stderr) = rewind($primary, $standby);
ok($ret, "pg_rewind rewinds from a source whose shares synced older blocks ($stderr)");
$primary->enable_streaming($standby);
$primary->start;
$standby->wait_for_replay_catchup($primary);
is(sql($primary, $rows_query), $t_rows, 'the rewound server reads t');
is(sql($primary, qq{SELECT test_bmap_rows_digest($copy)}), $digest,
	'and the copy');
$primary->stop;
$standby->stop;

# Copies made without an image on the source after the last common
# checkpoint, of pages t had before it, whose old blocks are then changed
# in place: the rewound server's replay of the copies would read the old
# blocks as copied from the source, changed.  pg_rewind refuses the source.
($primary, $standby, $t) = failover('rewind_copied');
sql($standby, q{CHECKPOINT; DELETE FROM t WHERE a % 50 = 0;});
$copy = sql($standby, qq{SELECT test_bmap_share($t)});
my $start = sql($standby, q{SELECT pg_current_wal_insert_lsn()});
sql($standby, q{DELETE FROM t WHERE a % 50 = 1});
is(sql($standby, qq{
	SELECT count(*) > 0 AND count(*) FILTER (WHERE block_fpi_length > 0) = 0
	FROM pg_get_wal_block_info('$start', pg_current_wal_insert_lsn(), false)
	WHERE resource_manager = 'BlockMap' AND record_type = 'REMAP'
	  AND relfilenode = $t AND relforknumber = 0}),
	't', 'the source copies t\'s pages without their images');
my $pages = sql($standby, qq{SELECT count(*) FROM test_bmap_map($copy)});
sql($standby, qq{CHECKPOINT;
	SELECT test_bmap_init_block($copy, b) FROM generate_series(0, $pages - 1) b});
($ret, $stderr) = rewind($primary, $standby);
ok(!$ret, 'pg_rewind refuses a source that changed blocks copied from without an image after the replay would begin');
like($stderr, qr/source server synced pages without logging them/,
	'saying why');
$primary->start;
is(sql($primary, q{SELECT count(*) FROM t WHERE a = -1}), '1',
	'the target, untouched, starts with its own rows');
$primary->stop;
$standby->stop;

done_testing();
