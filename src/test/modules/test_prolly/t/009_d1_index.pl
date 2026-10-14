# Copyright (c) 2026, PostgreSQL Global Development Group

# Prolly indexes on engine D1 tables: their trees are committed with the
# table's, so they survive a crash, a checkpoint and a restart, and a
# standby replays them; and a unique prolly index makes a second inserter
# of a key wait for the first and fail once it commits.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $conf = "default_table_access_method = prolly_d1\n"
  . "wal_consistency_checking = 'Prolly,ProllyStore'\n";

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', $conf);
$primary->start;
$primary->backup('base');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', $conf);
$standby->start;

$primary->safe_psql('postgres', q{
	CREATE TABLE t (id int PRIMARY KEY, k int, v text);
	INSERT INTO t SELECT g, g % 10, 'v' || g FROM generate_series(1, 1000) g;
	CREATE INDEX t_k ON t USING prolly (k);
	CREATE UNIQUE INDEX t_v ON t USING prolly (v);
	UPDATE t SET k = 42 WHERE id <= 10;
	DELETE FROM t WHERE id > 990;
});

my $by_index = q{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SELECT (SELECT count(*) FROM t WHERE k = 42),
		   (SELECT count(*) FROM t WHERE k = 1),
		   (SELECT id FROM t WHERE v = 'v500'),
		   (SELECT count(*) FROM (SELECT k FROM t ORDER BY k) s)};
my $expect = '10|98|500|990';
is($primary->safe_psql('postgres', $by_index), $expect, 'index scans on the primary');

$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $by_index), $expect,
	'the standby replays the index trees');

$primary->stop('immediate');
$primary->start;
is($primary->safe_psql('postgres', $by_index), $expect,
	'index trees survive a crash');
$primary->safe_psql('postgres', q{
	CHECKPOINT;
	INSERT INTO t VALUES (2000, 42, 'after checkpoint');
});
$primary->stop('immediate');
$primary->start;
is($primary->safe_psql('postgres', $by_index), '11|98|500|991',
	'commits on both sides of a checkpoint survive a crash');
$primary->restart;
is($primary->safe_psql('postgres', $by_index), '11|98|500|991',
	'and a restart');

# A second inserter of a key in a unique prolly index waits for the first.
my $s1 = $primary->background_psql('postgres');
my $s2 = $primary->background_psql('postgres', on_error_stop => 0);
$s1->query_safe(q{BEGIN; INSERT INTO t VALUES (3000, 1, 'same');});
$s2->query_until(qr/go/, "\\echo go\nINSERT INTO t VALUES (3001, 1, 'same');\n");
$primary->poll_query_until('postgres', q{
	SELECT count(*) > 0 FROM pg_locks WHERE locktype = 'prollytable' AND NOT granted
}) or die 'the second inserter did not wait';
$s1->query_safe('COMMIT');
$s2->query('SELECT 1');
like($s2->{stderr}, qr/duplicate key value violates unique constraint "t_v"/,
	'the second inserter fails once the first commits');
$s1->quit;
$s2->quit;
is($primary->safe_psql('postgres', q{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SELECT id FROM t WHERE v = 'same'}), '3000', 'the first row stays');

done_testing();
