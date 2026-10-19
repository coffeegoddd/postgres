# Copyright (c) 2026, PostgreSQL Global Development Group

# Cached snapshots must follow both changes to snapshot state during promotion:
# advancing latestCompletedXid and switching away from KnownAssignedXids.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'autovacuum = off');
$primary->start;
if (!$primary->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$primary->safe_psql('postgres',
	'CREATE EXTENSION injection_points; CREATE TABLE t (a int)');
$primary->backup('base');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

# Replay an assigned XID without its commit. Promotion will advance xmax
# past it, and then cease treating it as running.
my $writer = $primary->background_psql('postgres');
$writer->query_safe('BEGIN; INSERT INTO t VALUES (1)');
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$primary->wait_for_replay_catchup($standby);

my $reader = $standby->background_psql('postgres');
my $before = $reader->query_safe('SELECT pg_current_snapshot()');
my @points = ('promotion-after-wal-segment-cleanup',
	'promotion-after-recovery-state');
for my $point (@points)
{
	$standby->safe_psql('postgres',
		"SELECT injection_points_attach('$point', 'wait')");
}
$standby->safe_psql('postgres', 'SELECT pg_promote(false)');

my $catalog_reader;
for my $point (@points)
{
	$standby->wait_for_event('startup', $point);
	my $fresh = $standby->safe_psql('postgres', 'SELECT pg_current_snapshot()');
	isnt($fresh, $before, "snapshot changes at $point");
	is($reader->query_safe('SELECT pg_current_snapshot()'), $fresh,
		"cached snapshot agrees with a fresh snapshot at $point");
	$before = $fresh;
	if ($point eq $points[0])
	{
		# Connect after latestCompletedXid advances, but still in recovery.
		# Startup caches a catalog snapshot without a transaction snapshot.
		$catalog_reader = $standby->background_psql('postgres');
	}
	else
	{
		# First pin a fresh transaction snapshot after recovery ends, then
		# look up a table not yet cached by this backend. Reusing the older
		# catalog snapshot violates TransactionXmin <= RecentXmin.
		is( $catalog_reader->query_safe(
				'BEGIN ISOLATION LEVEL REPEATABLE READ;
				 SELECT 1;
				 SELECT count(*) FROM t;
				 COMMIT'),
			"1\n0",
			'catalog lookup after promotion preserves the snapshot xmin invariant');
	}
	$standby->safe_psql('postgres',
		"SELECT injection_points_wakeup('$point')");
}
$standby->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
	or die 'promotion did not complete';
is($reader->query_safe('SELECT count(*) FROM t'), '0',
	'uncommitted row remains invisible after promotion');
$reader->quit;
$catalog_reader->quit;
$writer->query_safe('ROLLBACK');
$writer->quit;
$standby->stop;
$primary->stop;
done_testing();
