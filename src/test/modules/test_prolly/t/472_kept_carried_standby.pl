# Copyright (c) 2026, PostgreSQL Global Development Group

# A standby replays resets back and forth between two kept commits, each
# restore carrying the other commit's snapshots into the storage it gives
# the table (checked page by page, wal_consistency_checking).  It serves
# each reset's rows, and, promoted, restores either commit, writing no row.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('kept_carried_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
wal_consistency_checking = all
wal_keep_size = 1GB
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
prolly.restore_min_changes = 0
});
$primary->start;
$primary->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('kept_carried_standby');
$standby->init_from_backup($primary, 'b', has_streaming => 1);
$standby->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }
sub counted
{
	my ($node, $name) = @_;
	return $node->safe_psql('postgres', qq{SELECT pg_stat_force_next_flush();
		SELECT coalesce((SELECT value FROM pg_stat_prolly WHERE name = '$name'), 0)});
}
my $digest = q{SELECT md5(string_agg(id || ':' || v, ',' ORDER BY id)) FROM app.t};

sql(q{CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int NOT NULL);
	CREATE INDEX t_v ON app.t (v);
	INSERT INTO app.t SELECT g, 0 FROM generate_series(1, 3000) g});
my %commit;
my %rows;
$commit{a} = sql(q{SELECT (dolt_commit('-Am', 'a'))[1]});
$rows{a} = sql($digest);
sql(q{UPDATE app.t SET v = 1 WHERE id <= 300});
$commit{b} = sql(q{SELECT (dolt_commit('-am', 'b'))[1]});
$rows{b} = sql($digest);

my $restored = counted($primary, 'relations_restored');
my $applied = counted($primary, 'apply_rows');
for my $i (1 .. 4)
{
	my $to = $i % 2 ? 'a' : 'b';

	sql(qq{UPDATE app.t SET v = v + 10 WHERE id % 7 = $i});
	sql(qq{SELECT dolt_reset('--hard', '$commit{$to}')});
	$primary->wait_for_replay_catchup($standby);
	is($standby->safe_psql('postgres', $digest), $rows{$to},
		"the standby replayed reset $i, to $to");
}
cmp_ok(counted($primary, 'relations_restored') - $restored, '>=', 4 * 2,
	'each reset restored the table and its index');
is(counted($primary, 'apply_rows') - $applied, 0, 'none wrote a row');

# promoted, either commit restored: b's snapshots carried, a's its own
$standby->promote;
for my $to ('a', 'b')
{
	$restored = counted($standby, 'relations_restored');
	$applied = counted($standby, 'apply_rows');
	$standby->safe_psql('postgres', qq{UPDATE app.t SET v = -1 WHERE id = 1;
		SELECT dolt_reset('--hard', '$commit{$to}')});
	cmp_ok(counted($standby, 'relations_restored'), '>', $restored,
		"promoted, a reset to $to restored");
	is(counted($standby, 'apply_rows') - $applied, 0, 'no row written');
	is($standby->safe_psql('postgres', $digest), $rows{$to}, "$to\'s rows");
}
is($standby->safe_psql('postgres', q{SELECT freed FROM prolly_page_sweep()}),
	'0', 'every block held by a map or a snapshot kept');

$standby->stop;
$primary->stop;
done_testing();
