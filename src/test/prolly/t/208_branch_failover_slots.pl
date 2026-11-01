# Copyright (c) 2026, PostgreSQL Global Development Group

# A failover slot made on a branch: a standby's synced copy keeps the
# branch, so after the standby is promoted, decoding from it carries that
# branch's changes and the shared schemas', never another branch's.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use ProllyTest;

my $primary = ProllyTest::new_node('primary', allows_streaming => 'logical');
$primary->append_conf('postgresql.conf', 'autovacuum = off');
$primary->start;
$primary->safe_psql('postgres', "SELECT pg_create_physical_replication_slot('sb_slot')");
$primary->backup('backup');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
my $conn = $primary->connstr . ' dbname=postgres';
$standby->append_conf('postgresql.conf', qq{
hot_standby_feedback = on
primary_slot_name = 'sb_slot'
primary_conninfo = '$conn'
});
$primary->append_conf('postgresql.conf', "synchronized_standby_slots = 'sb_slot'");
$primary->reload;
$standby->start;

my $b1 = $primary->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $b2 = $primary->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
sub on
{
	my ($node, $branch, $sql) = @_;
	return $node->safe_psql('postgres', "SET catalog_branch = $branch; $sql");
}
for my $b ($b1, $b2)
{
	on($primary, $b, 'CREATE SCHEMA app VERSIONED; CREATE TABLE app.t (id int PRIMARY KEY, v int)');
}
on($primary, $b1, "SELECT 1 FROM pg_create_logical_replication_slot('fo', 'test_decoding', false, false, true)");
# (a standby syncs a slot once the primary's has moved past where the
# standby's copy would begin: its catalog xmin moves on the second snapshot
# decoded after a transaction)
$primary->safe_psql('postgres', 'CREATE TABLE shared_t (i int)');
for (1 .. 2)
{
	$primary->safe_psql('postgres', 'INSERT INTO shared_t VALUES (1)');
	$primary->safe_psql('postgres', 'SELECT pg_log_standby_snapshot()');
	$primary->safe_psql('postgres',
		"SELECT 1 FROM pg_replication_slot_advance('fo', pg_current_wal_lsn())");
}
$primary->wait_for_replay_catchup($standby);

# the standby's copy, synced, keeps the slot's branch
$standby->safe_psql('postgres', 'SELECT pg_sync_replication_slots()');
is( $standby->safe_psql('postgres',
		"SELECT synced AND branch = $b1 FROM pg_replication_slots WHERE slot_name = 'fo'"),
	't', 'the synced copy keeps the slot\'s branch');

# promoted: changes on both branches, decoded from the copy on no branch,
# are the slot's branch's
on($primary, $b1, 'INSERT INTO app.t VALUES (1, 101)');
$primary->wait_for_replay_catchup($standby);
$standby->promote;
on($standby, $b1, 'INSERT INTO app.t VALUES (2, 102)');
on($standby, $b2, 'INSERT INTO app.t VALUES (3, 203)');
is( $standby->safe_psql('postgres',
		"SELECT string_agg(substring(data from 'v\\[integer\\]:([0-9]+)'), ',' ORDER BY lsn)
		 FROM pg_logical_slot_get_changes('fo', NULL, NULL) WHERE data LIKE 'table app.t%'"),
	'101,102', 'after failover the slot decodes its branch\'s changes alone');

$standby->stop;
$primary->stop;
done_testing();
