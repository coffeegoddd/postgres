# Copyright (c) 2026, PostgreSQL Global Development Group

# A move that restores another branch's kept roots holds them for the
# session's branch too, a copy of each root (a ROOT_SLOT record setting a
# slot, with the holders its subtrees gain).  A crash after the move, and a
# standby, replay those copies: the store's holder counts stay right, and
# the branch moves back by its own roots.  If one of a table's relations
# can't be held, those of its others already held are let go of again, not
# leaked.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('root_holds');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
wal_keep_size = '256MB'
wal_consistency_checking = 'all'
prolly.restore_min_changes = 0
});
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('root_holds_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub branch_sql
{
	my ($branch, $q) = @_;
	return $node->safe_psql("postgres/$branch", $q);
}

# the holder counts a sweep found wrong and set right, once what is let
# go of is drained
sub miscounted
{
	sql(q{SELECT count(*) FROM test_bmap_release_drain()});
	return sql(q{SELECT recounted FROM prolly_page_sweep()});
}

sql(q{
	CREATE SCHEMA rh VERSIONED;
	CREATE TABLE rh.t (id int PRIMARY KEY, v int NOT NULL);
	INSERT INTO rh.t SELECT g, 0 FROM generate_series(1, 2000) g;
	SELECT dolt_commit('-Am', 'c0');
	UPDATE rh.t SET v = 1 WHERE id % 10 = 1;
	SELECT dolt_commit('-am', 'c1');
	UPDATE rh.t SET v = 2 WHERE id % 10 = 2;
	SELECT dolt_commit('-am', 'c2');
});
my $c0 = sql(q{SELECT commit_hash FROM dolt_log WHERE message = 'c0'});
my $sum = q{SELECT sum(v) FROM rh.t};
is(miscounted(), '0', 'every holder counted');

# side moves to c0 by main's roots, holding them as its own
sql(q{SELECT dolt_branch('side')});
my $held = sql(q{SELECT held FROM test_bmap_roots()});
branch_sql('side', qq{SELECT dolt_reset('--hard', '$c0')});
is(branch_sql('side', $sum), '0', 'side is at c0');
my $after = sql(q{SELECT held FROM test_bmap_roots()});
cmp_ok($after, '>', $held, 'side holds roots of its own');

# a crash, then the standby: the copies replayed
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
$node->stop('immediate');
$node->start;
is(sql(q{SELECT held FROM test_bmap_roots()}), $after,
	'after a crash, the copies are held still');
is(miscounted(), '0', 'with their holders counted');
is($standby->safe_psql('postgres', q{SELECT held FROM test_bmap_roots()}),
	$after, 'the standby replayed the copies');

# main lets go of c0's roots (the branch deleted would; here its window
# its newest commit, side keeping its own), side moves to c2 and back: by
# its own
sql(q{SELECT dolt_history_window('main', 'min_commits', '1');
	UPDATE rh.t SET v = 3 WHERE id % 10 = 3; SELECT dolt_commit('-am', 'c3');
	UPDATE rh.t SET v = 4 WHERE id % 10 = 4; SELECT dolt_commit('-am', 'c4')});

# side's copies are its own, named as of side's table (its map), not of
# main's whose roots they copied: a sweep and VACUUM count them as side's
# own snapshots
my $side_t = branch_sql('side', q{SELECT pg_relation_filenode('rh.t')});
cmp_ok(sql(qq{SELECT test_bmap_roots_of($side_t)}), '>', 0,
	'side\'s copies are of its own table');
my $c2 = sql(q{SELECT commit_hash FROM dolt_log WHERE message = 'c2'});
branch_sql('side', qq{SELECT dolt_reset('--hard', '$c2')});
my $restored = sql(q{SELECT pg_stat_force_next_flush();
	SELECT value FROM pg_stat_prolly WHERE name = 'relations_restored'});
branch_sql('side', qq{SELECT dolt_reset('--hard', '$c0')});
cmp_ok(sql(q{SELECT pg_stat_force_next_flush();
	SELECT value FROM pg_stat_prolly WHERE name = 'relations_restored'}),
	'>', $restored, 'side moves back to c0 by its own roots');
is(branch_sql('side', $sum), '0', 'and has its rows');
is(miscounted(), '0', 'every holder counted');

# a hold refused for a table's second relation: the first's copy is let
# go of again, listed nowhere
my $before = sql(q{SELECT held FROM test_bmap_roots()});
sql(qq{SELECT dolt_branch('other', '$c0')});
branch_sql('other', $sum);
my $c4 = sql(q{SELECT commit_hash FROM dolt_log WHERE message = 'c4'});
my $copied = sql(q{SELECT pg_stat_force_next_flush();
	SELECT value FROM pg_stat_prolly WHERE name = 'bmap_roots_captured'});
branch_sql('other', qq{SELECT injection_points_set_local();
	SELECT injection_points_attach('refs-hold-refused', 'notice');
	SELECT dolt_reset('--hard', '$c4')});
is(branch_sql('other', $sum), sql($sum), 'the move is made all the same');
cmp_ok(sql(q{SELECT pg_stat_force_next_flush();
	SELECT value FROM pg_stat_prolly WHERE name = 'bmap_roots_captured'}),
	'>', $copied, 'by main\'s roots, a copy of one made');
# (the branch deleted lets go of every root its list holds: none other
# is left of it)
sql(q{SELECT dolt_branch('-D', 'other')});
sql(q{SELECT count(*) FROM test_bmap_release_drain()});
is(sql(q{SELECT held FROM test_bmap_roots()}), $before,
	'no copy is left held of the table, once the branch is deleted');
is(miscounted(), '0', 'every holder counted');

$standby->stop;
$node->stop;
done_testing();
