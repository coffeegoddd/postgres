# Copyright (c) 2026, PostgreSQL Global Development Group

# The history store's chunk log is WAL-logged: what was appended survives a
# crash, a standby replays it and reads it back, and its pages pass
# pg_checksums and a base backup's verification.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('chunk_log');
$primary->init(allows_streaming => 1, extra => ['--data-checksums']);
$primary->append_conf('postgresql.conf', 'autovacuum = off');
$primary->start;
$primary->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE chunks () USING prolly_chunks;
});
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('chunk_log_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

# appended after the base backup, so the standby has them from WAL alone
my $big = "convert_to(repeat('0123456789', 3000), 'UTF8')";
$primary->safe_psql('postgres', qq{
	SELECT test_chunk_log_append('chunks', '\\x616263');
	SELECT test_chunk_log_append('chunks', $big);
	SELECT test_chunk_log_append('chunks', '\\x7a');
});
my $check = qq{SELECT test_chunk_log_end('chunks'),
	test_chunk_log_read('chunks', 0, 3),
	test_chunk_log_read('chunks', 3, 30000) = $big,
	test_chunk_log_read('chunks', 30003, 1)};
# (one region of 32 pages of 8160 bytes)
my $want = '261120|\x616263|t|\x7a';

$primary->stop('immediate');
$primary->start;
is($primary->safe_psql('postgres', $check), $want, 'appends survive a crash');

$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $check), $want, 'a standby reads what it replayed');

# appending after the crash, a new backend's, goes into a region after the
# one the crash left
is($primary->safe_psql('postgres', "SELECT test_chunk_log_append('chunks', '\\x01')"),
	'261120', 'the next append begins after the recovered regions');

$primary->backup('after');
command_ok([ 'pg_verifybackup', $primary->backup_dir . '/after' ],
	'a base backup with the log verifies');
$standby->stop;
$primary->stop;
command_ok([ 'pg_checksums', '--check', '-D', $primary->data_dir ],
	'the log\'s pages pass pg_checksums');

done_testing();
