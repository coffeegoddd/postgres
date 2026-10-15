# Copyright (c) 2026, PostgreSQL Global Development Group

# Sharing block-mapped storage survives crashes and reaches standbys: a
# copy's map, the blocks' reference counts, and the source's pages as of
# the share, which reach the shared blocks even when they were only in the
# source's buffers.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('share_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
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
	CREATE SCHEMA cow VERSIONED;
	CREATE TABLE cow.t (a int8, b int8, c int8, d int8);
});

# The rows are only in buffers (no checkpoint since the backup) when the
# copy is made.
sql(q{INSERT INTO cow.t SELECT g, g, g, g FROM generate_series(1, 30000) g});
my $src = sql(q{SELECT pg_relation_filenode('cow.t')});
my $copy = sql(qq{SELECT test_bmap_share($src)});
my $digest = sql(qq{SELECT test_bmap_digest($src)});
my $refs = qq{SELECT string_agg(blkno || ':' || phys || ':' || refs, ',' ORDER BY blkno)
	FROM test_bmap_map($copy)};
my $map = sql($refs);
is(sql(qq{SELECT test_bmap_digest($copy)}), $digest, 'the copy reads the source\'s pages');

$primary->wait_for_replay_catchup($standby);
is(ssql(qq{SELECT test_bmap_digest($copy)}), $digest,
	'a standby\'s copy reads them too');
is(ssql($refs), $map, 'with the same blocks and counts');

# Crash recovery replays the share: the rows written since the checkpoint
# reach the shared blocks, and the counts come back.
$primary->stop('immediate');
$primary->start;
is(sql(qq{SELECT test_bmap_digest($copy)}), $digest, 'the copy survives a crash');
is(sql($refs), $map, 'so do its blocks and counts');

# Dropping the source frees nothing; dropping the copy then frees its blocks,
# on the standby as on the primary.
my $nblocks = sql(q{SELECT pg_relation_size('cow.t') / 8192});
my $free = sql(q{SELECT test_bmap_free_blocks()});
sql(q{DROP TABLE cow.t});
is(sql(q{SELECT test_bmap_free_blocks()}), $free, 'the copy keeps the source\'s blocks');
sql(qq{SELECT test_bmap_unlink($copy)});
is(sql(q{SELECT test_bmap_free_blocks()}) - $free, $nblocks,
	'the last map to go frees them');
$primary->wait_for_replay_catchup($standby);
is(ssql(q{SELECT test_bmap_free_blocks()}), sql(q{SELECT test_bmap_free_blocks()}),
	'the standby frees the same blocks');

$primary->stop('immediate');
$primary->start;
is(sql(q{SELECT test_bmap_free_blocks()}) - $free, $nblocks,
	'and so does crash recovery');

$standby->stop;
$primary->stop;
done_testing();
