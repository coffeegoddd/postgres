# Copyright (c) 2026, PostgreSQL Global Development Group

# Branch heads of engine D1 tables across crashes, checkpoints and streaming
# replication.
#
# A commit enqueues its new branch root in a ProllyCommit record before its
# commit record; after a crash, and on a standby, a branch's head is the
# root of the last enqueued transaction that committed.  Checkpoints and
# restartpoints save heads in global/prolly_heads.

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
	CREATE TABLE t (id int PRIMARY KEY, v text);
	INSERT INTO t SELECT g, 'v' || g FROM generate_series(1, 2000) g;
	CREATE TABLE k (a int);
	INSERT INTO k VALUES (1), (1), (2);
});

my $count = "SELECT count(*), sum(id) FROM t";
is($primary->safe_psql('postgres', $count), '2000|2001000', 'rows on the primary');

$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $count), '2000|2001000',
	'standby replays the commits');
is($standby->safe_psql('postgres', 'SELECT count(*) FROM k'), '3',
	'standby sees identical keyless rows');

# A transaction still open at a crash leaves nothing behind; committed
# ones survive.  (They write different tables: a D1 table has one writer
# at a time.)
my $open = $primary->background_psql('postgres');
$open->query_safe('BEGIN');
$open->query_safe(q{INSERT INTO k SELECT generate_series(1, 100)});
$primary->safe_psql('postgres', q{INSERT INTO t VALUES (3000, 'committed')});
$primary->stop('immediate');
$open->{run}->finish;
$primary->start;
is($primary->safe_psql('postgres', $count), '2001|2004000',
	'a crash keeps committed rows');
is($primary->safe_psql('postgres', 'SELECT count(*) FROM k'), '3',
	'a crash drops the open transaction');

# Commits after a checkpoint are replayed from the WAL; heads from before
# it come from the heads file.
$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->safe_psql('postgres', q{INSERT INTO t VALUES (3001, 'after checkpoint')});
$primary->stop('immediate');
$primary->start;
is($primary->safe_psql('postgres', $count), '2002|2007001',
	'commits on both sides of a checkpoint survive a crash');

# A clean restart reads the heads file.
$primary->restart;
is($primary->safe_psql('postgres', $count), '2002|2007001',
	'a clean restart keeps the heads');

# The standby follows, survives its own restart (with a restartpoint
# saving its heads), and keeps everything when promoted.
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $count), '2002|2007001',
	'standby follows the restarted primary');
$standby->safe_psql('postgres', 'CHECKPOINT');
$standby->restart;
is($standby->safe_psql('postgres', $count), '2002|2007001',
	'standby keeps its heads across a restartpoint and restart');
$standby->promote;
$standby->safe_psql('postgres', q{INSERT INTO t VALUES (4000, 'promoted')});
is($standby->safe_psql('postgres', $count), '2003|2011001',
	'promoted standby commits on top of the replayed head');

done_testing();
