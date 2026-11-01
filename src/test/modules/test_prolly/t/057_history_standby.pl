# Copyright (c) 2026, PostgreSQL Global Development Group

# The history store on a hot standby: its chunks read there, writes
# refused; a chunk whose freeing is in progress on the primary read
# without waiting for it; a query whose snapshot might have found a chunk
# before its freeing committed conflicts with VACUUM's taking the entry
# out (the chunk's bytes are then dead, for reuse), unless
# hot_standby_feedback holds VACUUM back.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('history_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
autovacuum = off
chunk_free_in_background = off
});
$primary->start;
$primary->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE flush (x int);
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	SELECT test_chunk_put('pg_history.chunks', 'pg_history.chunk_index', h(d), d)
		FROM (VALUES ('\x01'::bytea), ('\x02'), ('\x03')) v(d);
});
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('history_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', qq{
hot_standby_feedback = off
max_standby_streaming_delay = 0
wal_receiver_status_interval = 1
});
$standby->start;
$primary->wait_for_replay_catchup($standby);

my $get = sub {
	my ($d) = @_;
	return qq{SELECT test_chunk_get('pg_history.chunks', 'pg_history.chunk_index', h('$d'))};
};
my $has = sub {
	my ($d) = @_;
	return qq{SELECT test_chunk_has('pg_history.chunks', 'pg_history.chunk_index', h('$d'))};
};
# (VACUUM commits nothing: a write that does flushes its WAL, to replay;
# one leaving no catalog rows dead, whose pruning would conflict too)
my $flush = 'INSERT INTO flush VALUES (1)';
my $vacuum = sub {
	$primary->safe_psql('postgres', "VACUUM pg_history.chunk_index; $flush");
};
my $conflicts = q{SELECT confl_snapshot FROM pg_stat_database_conflicts WHERE datname = 'postgres'};
my $free = sub {
	my ($d) = @_;
	return qq{SELECT test_chunk_free('pg_history.chunks', 'pg_history.chunk_index', h('$d'))};
};

is($standby->safe_psql('postgres', $get->('\x01')), '\x01', 'a chunk read on the standby');
my ($ret, $stdout, $stderr) = $standby->psql('postgres',
	q{SELECT test_chunk_put('pg_history.chunks', 'pg_history.chunk_index', h('\x09'), '\x09')});
like($stderr, qr/cannot execute put a chunk in a read-only transaction/, 'and a put refused');

# a freeing in progress on the primary: read on the standby without waiting
my $freeing = $primary->background_psql('postgres');
$freeing->query_safe('BEGIN');
$freeing->query_safe($free->('\x01'));
$primary->safe_psql('postgres', $flush);
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', "SET statement_timeout = '5s'; " . $has->('\x01')),
	't', 'a chunk being freed on the primary is there on the standby, no waiting');
$freeing->query_safe('COMMIT');
$freeing->quit;
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $has->('\x01')), 'f', 'and gone once the freeing commits');

# a snapshot that found a chunk, its freeing committed, and VACUUM taking
# its entry out: the query conflicts
my $reader = $standby->background_psql('postgres', on_error_stop => 0);
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$reader->query_safe($get->('\x02'));
my $offset = -s $standby->logfile;
my $start_lsn = $primary->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
$primary->safe_psql('postgres', $free->('\x02'));
$vacuum->();
$primary->wait_for_replay_catchup($standby);
ok($standby->wait_for_log(
		qr/User query might have needed to see row versions that must be removed/, $offset),
	'VACUUM taking a freed chunk out conflicts with a standby query that might read it');
ok($standby->poll_query_until('postgres', "$conflicts", '1'), 'counted as a snapshot conflict');
$reader->quit;

# hot_standby_feedback holds VACUUM back instead
$standby->append_conf('postgresql.conf', 'hot_standby_feedback = on');
$standby->reload;
$reader = $standby->background_psql('postgres');
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$reader->query_safe($get->('\x03'));
my $xmin = $reader->query_safe(
	'SELECT backend_xmin FROM pg_stat_activity WHERE pid = pg_backend_pid()');
ok($primary->poll_query_until('postgres',
		qq{SELECT backend_xmin::text::int8 <= $xmin FROM pg_stat_replication}),
	'the standby\'s snapshot is fed back');
$primary->safe_psql('postgres', $free->('\x03'));
my $entries = q{SELECT entries FROM test_chunk_index_shape('pg_history.chunk_index')};
my $before = $primary->safe_psql('postgres', $entries);
$vacuum->();
is($primary->safe_psql('postgres', $entries), $before,
	'with feedback, VACUUM keeps the freed entry the standby might read');
$primary->wait_for_replay_catchup($standby);
is($reader->query_safe('SELECT 1'), '1', 'and the standby query goes on');
$reader->query_safe('COMMIT');
$reader->quit;
ok($primary->poll_query_until('postgres',
		qq{SELECT backend_xmin IS NULL OR backend_xmin::text::int8 > $xmin FROM pg_stat_replication}),
	'the standby\'s snapshot gone');
$vacuum->();
cmp_ok($primary->safe_psql('postgres', $entries), '<', $before, 'then VACUUM takes it out');
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $conflicts), '1', 'and no query conflicted');

# pg_waldump describes the record
my $end_lsn = $primary->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
($stdout, $stderr) = run_command([ 'pg_waldump', '--rmgr' => 'ChunkStore',
	'--path' => $primary->data_dir . '/pg_wal',
	'--start' => $start_lsn, '--end' => $end_lsn ]);
like($stdout, qr/rmgr: ChunkStore .*desc: INDEX_CLEANUP_INFO rel \d+\/\d+\/\d+; snapshotConflictHorizon: \d+/,
	'pg_waldump describes the cleanup records');

$standby->stop;
$primary->stop;
done_testing();
