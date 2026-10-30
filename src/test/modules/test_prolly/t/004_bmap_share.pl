# Copyright (c) 2026, PostgreSQL Global Development Group

# Sharing block-mapped storage survives crashes and reaches standbys: a
# copy's map, the blocks' reference counts, and the source's pages as of
# the share, which reach the shared blocks even when they were only in the
# source's buffers; and pages copied before they change.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('share_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
});
$primary->start;
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('share_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
# The standby evicts and writes no buffer on its own, so only replaying the
# share puts the source's pages in the shared blocks.
$standby->append_conf('postgresql.conf', qq{
shared_buffers = 32MB
bgwriter_lru_maxpages = 0
});
$standby->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }
sub ssql { return $standby->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA cow;
	CREATE TABLE cow.t (a int8, b int8, c int8, d int8);
});

# The rows are only in buffers (no checkpoint since the backup) when the
# copy is made.  The pages are compared as rows, without hint bits: a
# reader of the source (a scan, as the one below) may set
# HEAP_XMIN_COMMITTED in the source's buffers after the share wrote them,
# which the copy's buffers, the standby and a crash need not see, with no
# row changed.
sql(q{INSERT INTO cow.t SELECT g, g, g, g FROM generate_series(1, 30000) g});
my $src = sql(q{SELECT pg_relation_filenode('cow.t')});
my $copy = sql(qq{SELECT test_bmap_share($src)});
my $digest = sql(qq{SELECT test_bmap_rows_digest($src)});
my $source_rows = q{SELECT md5(string_agg(t::text, ',' ORDER BY a)) FROM cow.t};
my $srows = sql($source_rows);
my $refs = qq{SELECT string_agg(blkno || ':' || phys || ':' || refs, ',' ORDER BY blkno)
	FROM test_bmap_map($copy)};
my $map = sql($refs);
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $digest, 'the copy reads the source\'s pages');

$primary->wait_for_replay_catchup($standby);
is(ssql(qq{SELECT test_bmap_rows_digest($copy)}), $digest,
	'a standby\'s copy reads them too');
is(ssql($refs), $map, 'with the same blocks and counts');

# Crash recovery replays the share: the rows written since the checkpoint
# reach the shared blocks, and the counts come back.
$primary->stop('immediate');
$primary->start;
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $digest, 'the copy survives a crash');
is(sql($refs), $map, 'so do its blocks and counts');
is(sql($source_rows), $srows, 'and the source\'s rows');

# Changing the source copies its pages first: the copy keeps reading the
# rows it was made with, on the standby and after a crash too.  (Reading the
# source may set hint bits on pages still shared, which changes no row.)
my $rows = sql(qq{SELECT test_bmap_rows_digest($copy)});
sql(q{UPDATE cow.t SET b = -b});
my $sum = sql(q{SELECT sum(b) FROM cow.t});
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $rows, 'changing the source leaves the copy');
$primary->wait_for_replay_catchup($standby);
is(ssql(qq{SELECT test_bmap_rows_digest($copy)}), $rows, 'on the standby too');
is(ssql(q{SELECT sum(b) FROM cow.t}), $sum, 'which sees the source changed');
$primary->stop('immediate');
$primary->start;
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $rows, 'and after a crash');
is(sql(q{SELECT sum(b) FROM cow.t}), $sum, 'which keeps the source\'s change');

# Dropping the source frees only the blocks it copied to, and the map nodes
# only it holds; dropping the copy then frees the rest, on the standby as
# on the primary.
# (the visibility map's blocks, fork 2, are the maps' too)
my $src_own = sql(qq{SELECT count(*) FROM test_bmap_map($src)
	WHERE phys NOT IN (SELECT phys FROM test_bmap_map($copy))});
$src_own += sql(qq{SELECT count(*) FROM test_bmap_map($src, 2)
	WHERE phys NOT IN (SELECT phys FROM test_bmap_map($copy, 2))});
$src_own += sql(qq{SELECT count(DISTINCT block) FROM test_bmap_tree($src)
	WHERE block NOT IN (SELECT block FROM test_bmap_tree($copy))});
my $copy_blocks = sql(qq{SELECT count(*) FROM test_bmap_map($copy)});
$copy_blocks += sql(qq{SELECT count(*) FROM test_bmap_map($copy, 2)});
$copy_blocks += sql(qq{SELECT count(DISTINCT block) FROM test_bmap_tree($copy)});
my $free = sql(q{SELECT test_bmap_free_blocks()});
sql(q{DROP TABLE cow.t});
# (the drop queues what it held; the drain lets go of it)
sql(q{SELECT * FROM test_bmap_release_drain()});
is(sql(q{SELECT test_bmap_free_blocks()}) - $free, $src_own,
	'the copy keeps the blocks it shared');
sql(qq{SELECT test_bmap_unlink($copy)});
is(sql(q{SELECT test_bmap_free_blocks()}) - $free, $src_own + $copy_blocks,
	'the last map to go frees them');
$primary->wait_for_replay_catchup($standby);
is(ssql(q{SELECT test_bmap_free_blocks()}), sql(q{SELECT test_bmap_free_blocks()}),
	'the standby frees the same blocks');

$primary->stop('immediate');
$primary->start;
is(sql(q{SELECT test_bmap_free_blocks()}) - $free, $src_own + $copy_blocks,
	'and so does crash recovery');

$standby->stop;
$primary->stop;
done_testing();
