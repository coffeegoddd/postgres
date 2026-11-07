# Copyright (c) 2026, PostgreSQL Global Development Group

# Capture on a standby promoted: its slot synced to the standby as a failover slot, and capture
# going on from it on the new primary, every map its table's, what was written both before the
# slot last synced and after.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 'logical');
$primary->append_conf('postgresql.conf', "log_line_prefix = ''\nautovacuum = off\ncapture_to_trees = on\n");
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $b = $primary->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $b;";
$primary->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED; CREATE TABLE s.t (id integer PRIMARY KEY, v text);
	INSERT INTO s.t SELECT g, 'v' || g FROM generate_series(1, 100) g; CREATE TABLE s.k (a integer, v text)");
$primary->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker'})
  or die 'no worker';
my $slot = 'prolly_capture_' . $primary->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
is($primary->safe_psql('postgres', "SELECT failover FROM pg_replication_slots WHERE slot_name = '$slot'"), 't',
	"capture's slot is for failover");

sub caught_up
{
	my ($node) = @_;
	my $to = $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');
	$node->poll_query_until('postgres', qq{SELECT confirmed_flush_lsn >= '$to'::pg_lsn FROM pg_replication_slots WHERE slot_name = '$slot'})
	  or die 'capture did not catch up';
}

# (and a keyless table's, which holds how many there are of each row, so that what's told again
# and applied again would show)
sub same_map
{
	my ($node, $what) = @_;
	caught_up($node);
	for my $table ('t', 'k')
	{
		is( $node->safe_psql('postgres',
				"$on SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'capture/map/' || 's.$table'::regclass::oid"),
			$node->safe_psql('postgres', "$on SELECT test_ws_rows_hash('s', '$table')"), "$what: $table");
	}
}

same_map($primary, 'on the primary');

# a standby syncing slots
$primary->safe_psql('postgres', q{SELECT pg_create_physical_replication_slot('standby_slot')});
$primary->append_conf('postgresql.conf', "synchronized_standby_slots = 'standby_slot'");
$primary->reload;
$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
my $connstr = $primary->connstr;
$standby->append_conf('postgresql.conf', qq{hot_standby_feedback = on
primary_slot_name = 'standby_slot'
primary_conninfo = '$connstr dbname=postgres'
});
$standby->start;
$primary->safe_psql('postgres', "$on UPDATE s.t SET v = 'before the sync' WHERE id <= 10; INSERT INTO s.k VALUES (1, 'before the sync')");
caught_up($primary);
$primary->wait_for_replay_catchup($standby);
$standby->safe_psql('postgres', 'SELECT pg_sync_replication_slots()');
is($standby->safe_psql('postgres', "SELECT synced FROM pg_replication_slots WHERE slot_name = '$slot'"), 't', 'synced to the standby');

# written after the slot last synced, and captured on the primary, then the standby promoted
$primary->safe_psql('postgres', "$on UPDATE s.t SET v = 'after the sync' WHERE id BETWEEN 11 AND 20; DELETE FROM s.t WHERE id > 90;
	INSERT INTO s.k VALUES (1, 'after the sync'), (1, 'after the sync')");
caught_up($primary);
my $primary_at = $primary->safe_psql('postgres', "SELECT confirmed_flush_lsn FROM pg_replication_slots WHERE slot_name = '$slot'");
is($standby->safe_psql('postgres', "SELECT confirmed_flush_lsn < '$primary_at' FROM pg_replication_slots WHERE slot_name = '$slot'"), 't',
	"the standby's slot behind the primary's, so the promoted one is told again what the maps have");
$primary->wait_for_replay_catchup($standby);
$primary->stop;
$standby->promote;
$standby->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker'})
  or die 'no worker on the promoted standby';
same_map($standby, 'promoted: what was captured before and after the sync, as it is');
$standby->safe_psql('postgres', "$on INSERT INTO s.t VALUES (1000, 'on the new primary'); UPDATE s.t SET v = 'again' WHERE id = 1");
same_map($standby, 'and capture goes on from the slot');
my $root = $standby->safe_psql('postgres', "$on SELECT test_capture_barrier('s')");
is($root, $standby->safe_psql('postgres', "$on SELECT test_ws_root_capture('s')"), 'a commit from capture there is the tables');
unlike(slurp_file($standby->logfile), qr/^(PANIC|FATAL):.*capture|was invalidated, and is being made again/m,
	'from the slot it had, not one made again');

$standby->stop;
done_testing();
