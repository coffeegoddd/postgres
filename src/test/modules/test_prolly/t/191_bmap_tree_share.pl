# Copyright (c) 2026, PostgreSQL Global Development Group

# Sharing block maps: a share copies the root page alone, and the
# first change under a shared node copies the path to it (HOLD records for
# its children's counts, then the COPY).  A standby replays it all, and a
# crash at each record boundary of a path copy recovers both relations as
# they were: before the COPY, the children counted once too often, which a
# sweep finds once nothing else holds them; after it, the copy in place.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('bmap_tree_share');
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
my $standby = PostgreSQL::Test::Cluster->new('bmap_tree_share_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub rows
{
	my ($n, $rel) = @_;
	return $n->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($rel)});
}

sub standby_agrees
{
	my ($what, @rels) = @_;
	$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
	for my $rel (@rels)
	{
		is(rows($standby, $rel), rows($node, $rel),
			"$what: the standby reads $rel as the primary does");
	}
}

sub sweep_frees
{
	return sql(q{SELECT freed FROM prolly_page_sweep()});
}

# a write that copies a path, waiting at 'point', and a crash there once
# the records before are flushed
sub crash_at
{
	my ($point, $sql, @also) = @_;
	my $w = $node->background_psql('postgres');
	$w->query_safe(q{SELECT injection_points_set_local()});
	$w->query_safe(qq{SELECT injection_points_attach('$_', 'notice')})
	  for @also;
	$w->query_safe(qq{SELECT injection_points_attach('$point', 'wait')});
	$w->query_until(qr/writing/, qq{\\echo writing
$sql;
});
	$node->wait_for_event('client backend', $point);
	sql('CREATE TABLE IF NOT EXISTS flush_wal (); DROP TABLE flush_wal');
	$node->stop('immediate');
	$w->{run}->finish;
	$node->start;
}

sql(q{CREATE TABLE t (a int, b text) WITH (fillfactor = 10);
	INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(1, 6000) g});
my $rel = sql(q{SELECT pg_relation_filenode('t')});
my $orig = rows($node, $rel);
my $copy = sql(qq{SELECT test_bmap_share($rel)});
is(rows($node, $copy), $orig, 'the copy has the rows');
sql('CHECKPOINT');

# changes on both sides, under both subtrees
sql(qq{SELECT test_bmap_write($rel, 3, 1)});
sql(qq{SELECT test_bmap_write($rel, 700, 2)});
sql(qq{SELECT test_bmap_write($copy, 1500, 3)});
sql(q{UPDATE t SET b = 'y' WHERE a % 500 = 0});
sql(q{INSERT INTO t SELECT g, repeat('z', 200) FROM generate_series(6001, 8000) g});
my $src_rows = rows($node, $rel);
my $copy_rows = rows($node, $copy);
isnt($src_rows, $orig, 'the source changed');
standby_agrees('copies on write', $rel, $copy);
is(sweep_frees(), '0', 'a sweep finds nothing held by nothing');
$node->stop('immediate');
$node->start;
is(rows($node, $rel), $src_rows, 'the source recovers');
is(rows($node, $copy), $copy_rows, 'the copy recovers');
is(sweep_frees(), '0', 'and they hold every block they had');

# a second copy, and a crash after a path copy's HOLD, before its COPY
my $copy2 = sql(qq{SELECT test_bmap_share($rel)});
sql('CHECKPOINT');
crash_at('bmap-tree-held', "SELECT test_bmap_write($rel, 1200, 4)",
	'bmap-tree-hold-first');
is(rows($node, $rel), $src_rows, 'a crash between HOLD and COPY leaves the source');
is(rows($node, $copy2), $src_rows, 'and its copy');
is(sweep_frees(), '0', 'children counted once too often are still held');
sql(qq{SELECT test_bmap_write($rel, 1200, 4)});
standby_agrees('a copy after the crash', $rel, $copy2);

# a crash after a COPY, before the change that needed it
crash_at('bmap-tree-copied', "SELECT test_bmap_write($copy2, 30, 5)");
is(rows($node, $copy2), $src_rows, 'a crash after a COPY leaves the copy\'s rows');
sql(qq{SELECT test_bmap_write($copy2, 30, 5)});
my $copy2_rows = rows($node, $copy2);
$src_rows = rows($node, $rel);
standby_agrees('after the COPY', $rel, $copy2);

# every relation dropped: the counts the crash before the COPY left one too
# high are freed by a sweep
sql(qq{SELECT test_bmap_unlink($copy2)});
sql(qq{SELECT test_bmap_unlink($copy)});
sql(q{DROP TABLE t});
cmp_ok(sweep_frees(), '>', 0, 'a sweep frees what the lost COPY counted');
is(sweep_frees(), '0', 'and then nothing is held by nothing');
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));

$standby->stop;
$node->stop;
done_testing();
