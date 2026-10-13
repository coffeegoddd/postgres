# Copyright (c) 2026, PostgreSQL Global Development Group

# Crash recovery and streaming replay of the prolly node store and log.
#
# wal_consistency_checking compares every page a Prolly or ProllyStore
# record touches, after redo, with the full-page image the primary wrote,
# so any redo that doesn't reproduce the primary's page fails the replay.
# Enough nodes are written to split the store's hash map many times.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"wal_consistency_checking = 'Prolly,ProllyStore'");
$primary->start;
$primary->backup('base');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf',
	"wal_consistency_checking = 'Prolly,ProllyStore'");
$standby->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

# The store is created by the first write, after the base backup, so the
# standby replays its creation too.
is($primary->safe_psql('postgres', "SELECT prolly_store_write('global', 1, 6000)"),
	'6000', 'wrote nodes');
is($primary->safe_psql('postgres', "SELECT prolly_store_verify('global', 1, 6000)"),
	'6000', 'nodes found on the primary');

# The standby replays everything, checking each page.
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', "SELECT prolly_store_verify('global', 1, 6000)"),
	'6000', 'nodes found on the standby');

# More writes after a checkpoint, then a crash: recovery replays them.
# The transaction takes an xid so that its commit flushes the WAL, as an
# engine's commit will; WAL from a transaction without one is flushed
# asynchronously and may be lost in a crash, as for any such WAL.
$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->safe_psql('postgres',
	"BEGIN; SELECT prolly_store_write('global', 2, 3000); "
	  . "SELECT pg_current_xact_id(); COMMIT;");
$primary->stop('immediate');
$primary->start;
is($primary->safe_psql('postgres', "SELECT prolly_store_verify('global', 1, 6000)"),
	'6000', 'nodes from before the checkpoint survive a crash');
is($primary->safe_psql('postgres', "SELECT prolly_store_verify('global', 2, 3000)"),
	'3000', 'nodes from after the checkpoint survive a crash');

$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', "SELECT prolly_store_verify('global', 2, 3000)"),
	'3000', 'standby follows the restarted primary');

for my $node ($primary, $standby)
{
	my $log = slurp_file($node->logfile);
	unlike($log, qr/inconsistent page found|PANIC/,
		$node->name . ': no inconsistent pages in replay');
}

$standby->stop;
$primary->stop;
done_testing();
