# Copyright (c) 2026, PostgreSQL Global Development Group

# The page-store sync slots that let a share or snapshot holding nothing
# new skip its sync (#947), against what can happen meanwhile:
# - a slot taken by a sync reads as not durable until that sync completes
#   (it begins as one flush written, whatever came before);
# - a sync that began on a slot let go of meanwhile doesn't mark the slot
#   taken again synced: that one counts flushes written after the sync;
# - a flush finding every slot in flight or poisoned is still followed: by
#   a poisoned slot a checkpoint covers, let go of for it, or else among
#   the flushes no slot follows, while which no store is durable.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('store_sync_slot_races');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\ncheckpoint_timeout = 1h");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

my $plain_table = q{
	CREATE EXTENSION test_prolly;
	SET debug_block_mapped_heap = on;
	CREATE TABLE plain (a int, b text);
	INSERT INTO plain SELECT g, repeat('p', 100) FROM generate_series(1, 500) g;
	CREATE TABLE other AS SELECT * FROM plain;
	CREATE EXTENSION injection_points;};
for my $i (1 .. 70)
{
	$node->safe_psql('postgres', "CREATE DATABASE s$i");
	$node->safe_psql("s$i", $plain_table);
}

# The store syncs of snapshots of 'other' in database 'db' (plain may be
# held by a share waiting): one with blocks of its own if 'own', then one
# with none.
sub snapshots
{
	my ($db, $own) = @_;
	my $first = $own ? q{UPDATE other SET a = a + 1 WHERE a % 10 = 0;} : '';
	return $node->safe_psql($db, qq{
		CREATE TEMP TABLE before AS SELECT * FROM test_prolly_session_stats();
		$first
		SELECT test_bmap_snapshot_take(pg_relation_filenode('other'));
		CREATE TEMP TABLE mid AS SELECT * FROM test_prolly_session_stats();
		SELECT test_bmap_snapshot_take(pg_relation_filenode('other'));
		SELECT (m.value - b.value) || ',' || (s.value - m.value)
		FROM test_prolly_session_stats() s JOIN mid m USING (name)
			JOIN before b USING (name)
		WHERE s.name = 'bmap_store_syncs';
	}) =~ s/^.*\n//sr;
}

# A session whose next sync of a store waits once it is done.
sub paused_sync
{
	my ($db, $sql) = @_;
	my $s = $node->background_psql($db, on_error_stop => 0);
	$s->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('bmap-store-synced', 'wait')});
	$s->query_until(qr/started/, "\\echo started\n$sql;\n");
	$node->wait_for_event('client backend', 'bmap-store-synced');
	return $s;
}

sub wake
{
	my ($s, $point) = @_;
	$node->safe_psql('postgres', "SELECT injection_points_wakeup('$point')");
	$s->quit;
}

# Each store's first snapshot (its blocks its own), then a restart: no
# store has a slot.
$node->safe_psql("s$_", q{SELECT test_bmap_snapshot_take(pg_relation_filenode('plain'));
	SELECT test_bmap_snapshot_take(pg_relation_filenode('other'))})
  for (1, 2);
$node->restart;

sub slot_of
{
	my ($db) = @_;
	return $node->safe_psql($db,
		q{SELECT coalesce(written || ',' || synced || ',' || flushing, 'none')
		FROM test_bmap_store_sync()});
}

# 1. A slot taken by a sync (the store's first since the start, for a
# snapshot with nothing new): until the sync is done, another snapshot with
# nothing new syncs too.
is(slot_of('s1'), 'none', 's1 has no slot after the restart');
my $s = paused_sync('s1',
	q{SELECT test_bmap_snapshot_take(pg_relation_filenode('plain'))});
is(slot_of('s1'), '1,0,0',
	'the slot the sync took begins as one flush written, not synced');
is(snapshots('s1', 0), '1,0',
	'while that sync runs, a snapshot with nothing new syncs, then skips');
wake($s, 'bmap-store-synced');

# 2. A sync of s2 done, not yet counted, while s2's slot is let go of (64
# other stores synced) and taken again, by as many flushes written as the
# sync began with: the sync doesn't count for the slot taken again, whose
# flushes were written after it.
sub share_rolled_back
{
	my ($db, $r) = @_;
	$node->safe_psql($db, qq{UPDATE plain SET a = a + 1 WHERE a % 10 = $r;
		BEGIN; SELECT test_bmap_share(pg_relation_filenode('plain'), true); ROLLBACK});
}
share_rolled_back('s2', 1);
$s = paused_sync('s2',
	q{SELECT test_bmap_snapshot_take(pg_relation_filenode('other'))});
my ($began) = split /,/, slot_of('s2');
$node->safe_psql("s$_", q{SELECT test_bmap_snapshot_take(pg_relation_filenode('other'))})
  for (3 .. 66);
is(slot_of('s2'), 'none', "s2's slot was let go of");
for my $r (2 .. 9)
{
	last if slot_of('s2') =~ /^(\d+),/ && $1 >= $began;
	share_rolled_back('s2', $r);
}
like(slot_of('s2'), qr/^$began,0,0$/,
	"s2's slot taken again counts as many flushes written as the sync began with");
wake($s, 'bmap-store-synced');
like(slot_of('s2'), qr/^$began,0,0$/, 'the sync that began before marks none synced');
is(snapshots('s2', 0), '1,0',
	'a snapshot with nothing new syncs, then skips');

# 3. Every slot poisoned: 64 shares given up part way, in s3..s66.
sub poison_all
{
	for my $i (3 .. 66)
	{
		my ($ret, $out, $err) = $node->psql("s$i", q{
			UPDATE plain SET a = a + 1 WHERE a % 10 = 5;
			SELECT injection_points_set_local();
			SELECT injection_points_attach('bmap-share-map-page', 'error');
			SELECT test_bmap_share(pg_relation_filenode('plain'), true)});
		die "the share in s$i didn't fail: $err" unless $err =~ /bmap-share-map-page/;
	}
}

# A share of s67's, waiting with its counts raised and its blocks unwritten.
sub paused_share
{
	my ($db) = @_;
	my $w = $node->background_psql($db, on_error_stop => 0);
	$w->query_safe(q{UPDATE plain SET a = a + 1 WHERE a % 10 = 6;
		SELECT injection_points_set_local();
		SELECT injection_points_attach('bmap-share-map-page', 'wait')});
	$w->query_until(qr/started/, q{\echo started
		BEGIN;
		SELECT test_bmap_share(pg_relation_filenode('plain'), true);
	});
	$node->wait_for_event('client backend', 'bmap-share-map-page');
	return $w;
}

# (a) a checkpoint covering the poison first: the share takes a poisoned
# slot, let go of for it, and its store isn't durable meanwhile.
poison_all();
$node->safe_psql('postgres', 'CHECKPOINT');
my $w = paused_share('s67');
is(snapshots('s3', 0), '1,0', 'another store, its poison covered, syncs then skips');
is(snapshots('s67', 1), '1,1',
	'a share in flight that took a poisoned slot: its store syncs every time');
wake($w, 'bmap-share-map-page');

# (b) a checkpoint covering the poison only after a commit's keep began,
# which found no slot at all (paused once a snapshot's slot was taken,
# holding no buffer): no store is durable while its flush is in flight.
$node->safe_psql('s68', q{
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int);
	INSERT INTO app.t SELECT g, 0 FROM generate_series(1, 2000) g;
	SELECT dolt_commit('-Am', 'base')});
poison_all();
$w = $node->background_psql('s68', on_error_stop => 0);
$w->query_safe(q{UPDATE app.t SET v = v + 1 WHERE id % 10 = 0;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-snapshot-slot-taken', 'wait')});
$w->query_until(qr/started/, q{\echo started
	SELECT dolt_commit('-am', 'kept');
});
$node->wait_for_event('client backend', 'bmap-snapshot-slot-taken');
$node->safe_psql('postgres', 'CHECKPOINT');
is(snapshots('s4', 0), '1,1',
	'a flush no slot follows in flight: another store syncs every time');
is(snapshots('s68', 1), '1,1', 'and so does its own');
$node->safe_psql('postgres', q{SELECT injection_points_detach('bmap-snapshot-slot-taken');
	SELECT injection_points_wakeup('bmap-snapshot-slot-taken')});
$w->quit;
is(snapshots('s4', 0), '0,0', 'once it is done, the other store skips again');

$node->stop;
done_testing();
