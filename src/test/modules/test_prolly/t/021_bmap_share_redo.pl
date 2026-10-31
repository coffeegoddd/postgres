# Copyright (c) 2026, PostgreSQL Global Development Group

# Redo of a share writes out the source's cached pages by looking up its
# own blocks, those the share logged as images, rather than by visiting
# every buffer: a standby or crash recovery replaying many shares with a
# large shared_buffers would otherwise pay for the whole buffer pool at
# each one, a share right after a checkpoint included.  The copy, the
# standby and crash recovery still read the source's pages as of the share.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('share_redo_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
log_min_messages = debug1
});
$primary->start;
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('share_redo_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }
sub ssql { return $standby->safe_psql('postgres', $_[0]); }

# Data checksums let redo trust the map pages it reads (a torn one is
# found), which its walk needs.
is(sql(q{SHOW data_checksums}), 'on', 'data checksums are on');

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE t (a int, b text);
	INSERT INTO t SELECT g, repeat('x', 100) FROM generate_series(1, 5000) g;
});
my $src = sql(q{SELECT pg_relation_filenode('t')});
my $pages = sql(q{SELECT pg_relation_size('t') / 8192});
my $path = sql(q{SELECT pg_relation_filepath('t')});

# The rows are only in buffers when the copy is made, and their pages are
# the source's own: the share logs each.
my $log_offset = -s $standby->logfile;
my $copy = sql(qq{SELECT test_bmap_share($src)});
my $digest = sql(qq{SELECT test_bmap_rows_digest($src)});
$primary->wait_for_replay_catchup($standby);
ok( $standby->log_contains(
		qr/redo of a share of \Q$path\E writes out its own pages: $pages,/,
		$log_offset),
	'the standby writes out the source\'s own pages, looked up');
ok( !$standby->log_contains(
		qr/redo of a share of \Q$path\E writes out every buffer of it/,
		$log_offset),
	'visiting no other buffer');
# Each count page is read once by the walk, not once for every block it
# counts: the source's few dozen blocks are counted on one or two pages.
my ($ref_pages) = slurp_file($standby->logfile, $log_offset) =~
  /redo of a share of \Q$path\E writes out its own pages: \d+, found reading \d+ nodes of its map and (\d+) count pages/;
ok(defined $ref_pages && $ref_pages <= 2,
	'reading each count page once (' . ($ref_pages // 'none') . " for $pages pages)");

# The page store's and its counts' lengths, asked once each for the walk,
# not once for every node it reads: a source of thousands of pages, whose
# walk reads several.
sql(q{
	CREATE TABLE big (a int, b text);
	INSERT INTO big SELECT g, repeat('b', 100) FROM generate_series(1, 3000 * 60) g;
});
my $bpath = sql(q{SELECT pg_relation_filepath('big')});
$log_offset = -s $standby->logfile;
sql(q{SELECT test_bmap_share(pg_relation_filenode('big'))});
$primary->wait_for_replay_catchup($standby);
my ($bnodes, $lengths) = slurp_file($standby->logfile, $log_offset) =~
  /redo of a share of \Q$bpath\E writes out its own pages: \d+, found reading (\d+) nodes of its map and \d+ count pages, asking (\d+) files' lengths/;
cmp_ok($bnodes // 0, '>=', 3, 'a big source\'s walk reads several nodes');
ok(defined $lengths && $lengths <= 2,
	'asking each file\'s length once (' . ($lengths // 'none') . " for $bnodes nodes)");
is(ssql(qq{SELECT test_bmap_rows_digest($copy)}), $digest,
	'the standby\'s copy reads the source\'s pages');

# A second share of the same source: its pages are shared now, none its
# own, so redo writes out none.
$log_offset = -s $standby->logfile;
my $copy2 = sql(qq{SELECT test_bmap_share($src)});
# (flushed: a share outside a transaction doesn't flush its WAL, and this
# one logs nothing else that would)
sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
$primary->wait_for_replay_catchup($standby);
ok( $standby->log_contains(
		qr/redo of a share of \Q$path\E writes out its own pages: 0,/,
		$log_offset),
	'a share of pages shared already writes out none');
is(ssql(qq{SELECT test_bmap_rows_digest($copy2)}), $digest,
	'and its copy reads them too');

# A share right after a checkpoint, the first change of its source's header
# since, logs the header as an image: the record keeps the header's delta
# with it, so redo still has the source's length and epoch for its walk.
sql(q{
	CREATE TABLE u (a int, b text);
	INSERT INTO u SELECT g, repeat('u', 100) FROM generate_series(1, 2000) g;
});
my $usrc = sql(q{SELECT pg_relation_filenode('u')});
my $upages = sql(q{SELECT pg_relation_size('u') / 8192});
my $upath = sql(q{SELECT pg_relation_filepath('u')});
sql('CHECKPOINT');
$log_offset = -s $standby->logfile;
my $ucopy = sql(qq{SELECT test_bmap_share($usrc)});
my $udigest = sql(qq{SELECT test_bmap_rows_digest($usrc)});
$primary->wait_for_replay_catchup($standby);
ok( $standby->log_contains(
		qr/redo of a share of \Q$upath\E writes out its own pages: $upages,/,
		$log_offset),
	'a share after a checkpoint writes out the source\'s own pages, looked up');
ok( !$standby->log_contains(
		qr/redo of a share of \Q$upath\E writes out every buffer of it/,
		$log_offset),
	'visiting no other buffer');
is(ssql(qq{SELECT test_bmap_rows_digest($ucopy)}), $udigest,
	'and the standby\'s copy reads the source\'s pages');

# Changing some pages after the share makes them the source's own again;
# the next share's redo, in crash recovery, writes out only those.
sql(q{UPDATE t SET b = 'y' WHERE a <= 40});
my $changed = sql(qq{SELECT count(*) FROM test_bmap_map($src) WHERE refs = 0});
cmp_ok($changed, '>', 0, 'the update gave the source pages of its own');
cmp_ok($changed, '<', $pages, 'but not all of them');
# (and its visibility map's pages, the update's own too, written out alike)
$changed += sql(qq{SELECT count(*) FROM test_bmap_map($src, 2) WHERE refs = 0});
my $copy3 = sql(qq{SELECT test_bmap_share($src)});
my $digest3 = sql(qq{SELECT test_bmap_rows_digest($src)});
$log_offset = -s $primary->logfile;
$primary->stop('immediate');
$primary->start;
ok( $primary->log_contains(
		qr/redo of a share of \Q$path\E writes out its own pages: $changed,/,
		$log_offset),
	'crash recovery writes out the pages the source changed since');
is(sql(qq{SELECT test_bmap_rows_digest($copy3)}), $digest3,
	'the copy reads them after the crash');
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $digest,
	'and the first copy its own rows');
is(sql(q{SELECT count(*) FROM t WHERE b = 'y'}), '40',
	'and the source keeps its change');

$standby->stop;
$primary->stop;
done_testing();
