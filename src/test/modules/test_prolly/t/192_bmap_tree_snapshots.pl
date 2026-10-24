# Copyright (c) 2026, PostgreSQL Global Development Group

# Snapshots of tree block maps survive crashes and reach standbys: a
# snapshot kept before a crash still has the pages the relation had then,
# on the primary after recovery and on a standby; a crash after a
# snapshot's slot is cleared, before its subtrees are let go of, leaves
# their counts a holder too high, which a sweep finds once nothing else
# holds them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('bmap_tree_snapshots');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
shared_buffers = 16MB
checkpoint_timeout = 1h
wal_keep_size = '1GB'
wal_consistency_checking = 'all'
});
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('bmap_tree_snapshots_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub rows
{
	my ($n, $rel) = @_;
	return $n->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($rel)});
}

sub sweep_frees
{
	return sql(q{SELECT freed FROM prolly_page_sweep()});
}

sql(q{CREATE TABLE t (a int, b text) WITH (fillfactor = 10);
	INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(1, 6000) g});
my $rel = sql(q{SELECT pg_relation_filenode('t')});
my $then = rows($node, $rel);
sql('CHECKPOINT');
my $snap = sql(qq{SELECT test_bmap_snapshot_take($rel)});
sql(q{UPDATE t SET b = 'y' WHERE a % 50 = 0});
sql(q{DELETE FROM t WHERE a > 5000});
# (no VACUUM: it would copy every page the snapshot alone shares, to set
# it all-visible, leaving none shared for the crash below to count twice)
my $now = rows($node, $rel);
isnt($now, $then, 'the table changed after the snapshot');

# after a crash, the snapshot still has the pages it had
$node->stop('immediate');
$node->start;
is(rows($node, $rel), $now, 'the table recovers');
is(sql(qq{SELECT count(*) FROM test_bmap_snapshots($rel) WHERE id = $snap}), '1',
	'the snapshot is kept');
my $copy = sql(qq{SELECT test_bmap_share_snapshot($rel, $snap)});
is(rows($node, $copy), $then, 'a relation made of it has the pages then');
is(sweep_frees(), '0', 'a sweep finds nothing held by nothing');

# a standby has it all too
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is(rows($standby, $copy), $then, 'the standby reads the snapshot\'s copy');
is(rows($standby, $rel), $now, 'and the table');
# (an unlink logs no drop: the map goes at the checkpoint, not replayed)
sql(qq{SELECT test_bmap_unlink($copy)});
sql("CHECKPOINT");

# a crash after the slot is cleared, before the subtrees are let go of
my $w = $node->background_psql('postgres');
$w->query_safe(q{SELECT injection_points_set_local()});
$w->query_safe(q{SELECT injection_points_attach('bmap-snapshot-cleared', 'wait')});
$w->query_until(qr/releasing/, qq{\\echo releasing
SELECT test_bmap_snapshot_release($rel, $snap);
});
$node->wait_for_event('client backend', 'bmap-snapshot-cleared');
sql('CREATE TABLE flush_wal ()');
$node->stop('immediate');
$w->{run}->finish;
$node->start;
is(sql(qq{SELECT count(*) FROM test_bmap_snapshots($rel)}), '0',
	'the snapshot is let go of');
is(rows($node, $rel), $now, 'the table is as it was');
cmp_ok(sweep_frees(), '>', 0,
	'a sweep frees the pages only the snapshot held');
is(sweep_frees(), '0', 'and then nothing is held by nothing');

# a drop lets go of the snapshots its relation keeps (another table: the
# crash above left t's shared blocks counted once too often, for a sweep
# after t's drop)
sql(q{CREATE TABLE t2 (a int, b text) WITH (fillfactor = 10);
	INSERT INTO t2 SELECT g, repeat('x', 200) FROM generate_series(1, 6000) g});
sql(q{SELECT test_bmap_snapshot_take(pg_relation_filenode('t2'))});
sql(q{UPDATE t2 SET b = 'z' WHERE a % 7 = 0});
sql(q{SELECT test_bmap_snapshot_take(pg_relation_filenode('t2'))});
sql(q{DROP TABLE t2});
is(sweep_frees(), '0', 'a drop frees what its snapshots alone held');
sql(q{DROP TABLE t});
cmp_ok(sweep_frees(), '>', 0,
	'with t dropped, a sweep frees the blocks it counted once too often');
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));

$standby->stop;
$node->stop;
done_testing();
